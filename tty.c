#include "tty.h"

#include <sys/ioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int cached = -1;

void tty_set_color(int mode)
{
    cached = mode;
}

bool use_color(void)
{
    if (cached < 0)
        cached = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;
    return cached != 0;
}

bool stdin_is_tty(void)
{
    return isatty(STDIN_FILENO) != 0;
}

void discard_typeahead(void)
{
    if (isatty(STDIN_FILENO))
        tcflush(STDIN_FILENO, TCIFLUSH);
}

bool read_line(char *buf, size_t sz)
{
    fflush(stdout);
    if (fgets(buf, (int)sz, stdin) == NULL)
        return false;
    size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n') {
        buf[--n] = '\0';
    } else {
        /* discard the rest of an over-long line */
        int c;
        while ((c = getchar()) != EOF && c != '\n')
            ;
    }
    return true;
}

bool ask_yesno(const char *prompt, bool default_yes, bool noconfirm)
{
    printf("%s%s%s %s ", C_BLUE, prompt, C_END, default_yes ? "[Y/n]" : "[y/N]");
    if (noconfirm) {
        puts(default_yes ? "y" : "n");
        return default_yes;
    }
    char buf[32];
    if (!read_line(buf, sizeof buf))
        return false;
    if (buf[0] == '\0')
        return default_yes;
    return buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 's' || buf[0] == 'S';
}

bool ask_approval(const char *prompt)
{
    if (!stdin_is_tty()) {
        fprintf(stderr, "error: approval requires an interactive terminal; refusing\n");
        return false;
    }
    char buf[64];
    printf("%s%s%s [y/N] ", C_BLUE, prompt, C_END);
    if (!read_line(buf, sizeof buf))
        return false;
    return strcmp(buf, "y") == 0 || strcmp(buf, "Y") == 0 ||
           strcmp(buf, "yes") == 0 || strcmp(buf, "s") == 0 ||
           strcmp(buf, "sim") == 0;
}

size_t tty_columns(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    const char *c = getenv("COLUMNS");
    long n = c ? strtol(c, NULL, 10) : 0;
    return n > 0 && n < 10000 ? (size_t)n : 80;
}
