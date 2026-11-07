#include "mascot.h"
#include "buf.h"
#include "tty.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * 16x16 pixels. '.' transparent, B ink body, L shine, D dark ink (shading
 * and drips), W eye white, P pupil.
 */
static const char *const ART[] = {
    "......BBBB......",
    "....BBBBBBBB....",
    "...BLLBBBBBBB...",
    "..BLLBBBBBBBBB..",
    "..BLBWWWBBWWWB..",
    ".BBBWWWWBWWWWBB.",
    ".BBBWWPPBWWPPBB.",
    ".BBBBWPPBBWPPBB.",
    ".BBBBBBBBBBBBBD.",
    ".BBBBBBBBBBBBBD.",
    ".BBBBBBBBBBBBDD.",
    ".DBBBBBBBBBBBDD.",
    ".DD.DBBBD.DBBD..",
    ".D...DBD...DD...",
    "......D.....D...",
    "......D.........",
};
#define ART_W 16
#define ART_H 16

/* Same colors as the terminal palette below, for the SVG. */
static const char *hex_of(char c)
{
    switch (c) {
    case 'B': return "#5f5fd7";
    case 'L': return "#87afff";
    case 'D': return "#5f00af";
    case 'W': return "#eeeeee";
    case 'P': return "#00005f";
    default:  return NULL;
    }
}

void mascot_write_svg(FILE *fp, unsigned scale)
{
    fprintf(fp, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%u\" height=\"%u\" "
                "viewBox=\"0 0 %d %d\" shape-rendering=\"crispEdges\">\n"
                "<title>inkaur mascot</title>\n",
            scale * ART_W, scale * ART_H, ART_W, ART_H);
    for (int y = 0; y < ART_H; y++) {
        for (int x = 0; x < ART_W;) {   /* one rect per run of equal pixels */
            const char *col = hex_of(ART[y][x]);
            int run = 1;
            while (x + run < ART_W && ART[y][x + run] == ART[y][x])
                run++;
            if (col)
                fprintf(fp, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"1\" fill=\"%s\"/>\n",
                        x, y, run, col);
            x += run;
        }
    }
    fputs("</svg>\n", fp);
}

/* xterm 256-color palette indexes */
static int color_of(char c)
{
    switch (c) {
    case 'B': return 62;    /* ink blue   #5f5fd7 */
    case 'L': return 111;   /* shine      #87afff */
    case 'D': return 55;    /* dark ink   #5f00af */
    case 'W': return 255;   /* eye white  #eeeeee */
    case 'P': return 17;    /* pupil      #00005f */
    default:  return -1;
    }
}

/* Terminal width: the tty itself, else $COLUMNS, else 80. */
static size_t term_columns(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    const char *c = getenv("COLUMNS");
    long n = c ? strtol(c, NULL, 10) : 0;
    return n > 0 && n < 10000 ? (size_t)n : 80;
}

/*
 * Word-wrap lines to width. A line starting with "- " continues with a
 * two-space hanging indent. The wrapped lines are appended to out.
 */
static void wrap_lines(const char *const *lines, size_t n, size_t width, StrList *out)
{
    if (width < 10)
        width = 10;
    for (size_t i = 0; i < n; i++) {
        const char *indent = strncmp(lines[i], "- ", 2) == 0 ? "  " : "";
        StrBuf cur;
        sb_init(&cur, 4096);
        const char *p = lines[i];
        while (*p) {
            while (*p == ' ' && cur.len)
                p++;
            size_t w = strcspn(p, " ");
            if (p[0] == '-' && p[1] == ' ' && cur.len == 0)
                w = 1;   /* keep the "- " bullet glued to the first word */
            if (cur.len && cur.len + 1 + w > width) {
                sl_push(out, cur.data);
                sb_reset(&cur);
                sb_puts(&cur, indent);
            }
            if (cur.len && cur.data[cur.len - 1] != ' ')
                sb_putc(&cur, ' ');
            sb_append(&cur, p, w);
            p += w;
        }
        if (cur.len)
            sl_push(out, cur.data);
        sb_free(&cur);
    }
}

static void print_art_row(size_t r)
{
    fputs("  ", stdout);
    for (size_t x = 0; x < ART_W; x++) {
        int top = color_of(ART[2 * r][x]), bot = color_of(ART[2 * r + 1][x]);
        if (top < 0 && bot < 0)
            fputs(" ", stdout);
        else if (top < 0)
            printf("\033[38;5;%dm\xe2\x96\x84\033[0m", bot);           /* ▄ */
        else if (bot < 0)
            printf("\033[38;5;%dm\xe2\x96\x80\033[0m", top);           /* ▀ */
        else
            printf("\033[38;5;%d;48;5;%dm\xe2\x96\x80\033[0m", top, bot);
    }
}

#define ART_LEFT 2    /* spaces before the art */
#define ART_GAP 3     /* spaces between the art and the text */
#define SIDE_MIN 40   /* text columns needed to put it beside the art */

void mascot_print(const char *title, const char *const *lines, size_t nlines)
{
    size_t cols = term_columns();
    if (!use_color()) {   /* pipes, NO_COLOR: plain text for scripts, never wrapped */
        puts(title);
        for (size_t i = 0; i < nlines; i++)
            puts(lines[i]);
        return;
    }
    size_t rows = ART_H / 2, art_cols = ART_LEFT + ART_W;
    bool side = cols >= art_cols + ART_GAP + SIDE_MIN;
    bool art = cols >= art_cols + 1;
    size_t width = side ? cols - art_cols - ART_GAP : cols;

    StrList text;
    sl_init(&text);
    sl_push(&text, title);   /* the title is short and never wrapped */
    size_t title_lines = 1;
    wrap_lines(lines, nlines, width, &text);

    if (!side) {   /* narrow: art on top (if it fits), text below */
        for (size_t r = 0; art && r < rows; r++) {
            print_art_row(r);
            putchar('\n');
        }
        for (size_t i = 0; i < text.n; i++)
            printf("%s%s%s\n", i < title_lines ? C_BOLD : "", text.v[i],
                   i < title_lines ? C_END : "");
        sl_free(&text);
        return;
    }
    size_t first = text.n < rows ? (rows - text.n) / 2 : 0;
    for (size_t r = 0; r < rows || r - first < text.n; r++) {
        if (r < rows)
            print_art_row(r);
        else
            printf("%*s", (int)art_cols, "");
        if (r >= first && r - first < text.n) {
            size_t i = r - first;
            printf("%*s%s%s%s", ART_GAP, "", i < title_lines ? C_BOLD : "", text.v[i],
                   i < title_lines ? C_END : "");
        }
        putchar('\n');
    }
    sl_free(&text);
}
