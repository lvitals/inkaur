#include "mascot.h"
#include "tty.h"

#include <stdio.h>
#include <string.h>

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

void mascot_print(const char *const *lines, size_t nlines)
{
    if (!use_color()) {
        for (size_t i = 0; i < nlines; i++)
            puts(lines[i]);
        return;
    }
    size_t rows = ART_H / 2;
    size_t first = nlines < rows ? (rows - nlines) / 2 : 0;
    for (size_t r = 0; r < rows || r - first < nlines; r++) {
        if (r < rows) {
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
        } else {
            printf("%*s", ART_W + 2, "");
        }
        if (r >= first && r - first < nlines)
            printf("   %s", lines[r - first]);
        putchar('\n');
    }
}
