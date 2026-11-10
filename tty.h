#ifndef TTY_H
#define TTY_H

#include <stdbool.h>
#include <stddef.h>

bool use_color(void);          /* stdout is a tty and NO_COLOR unset */
void tty_set_color(int mode);  /* -1 auto, 0 never, 1 always (--color) */
bool stdin_is_tty(void);
/* Width of the terminal on stdout ($COLUMNS, else 80). */
size_t tty_columns(void);

/* Generic question. With noconfirm returns default_yes without asking. */
bool ask_yesno(const char *prompt, bool default_yes, bool noconfirm);

/*
 * Security yes/no (key import, .SRCINFO mismatch). NEVER skipped by
 * --noconfirm and refused when stdin is not a terminal (no "yes |" piping).
 * Package approval uses the menu in review.h (ask_decision).
 */
bool ask_approval(const char *prompt);

/* Drop keys typed before a prompt was shown (e.g. extra ENTERs in the pager),
 * so they cannot answer a security question by accident. */
void discard_typeahead(void);

/* Read one line from stdin (trailing newline stripped). */
bool read_line(char *buf, size_t sz);

#define C_BOLD  (use_color() ? "\033[1m" : "")
#define C_RED   (use_color() ? "\033[1;91m" : "")
#define C_GREEN (use_color() ? "\033[1;92m" : "")
#define C_YEL   (use_color() ? "\033[1;93m" : "")
#define C_BLUE  (use_color() ? "\033[1;94m" : "")
#define C_END   (use_color() ? "\033[0m" : "")

#endif /* TTY_H */
