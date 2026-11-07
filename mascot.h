#ifndef MASCOT_H
#define MASCOT_H

#include <stddef.h>

/*
 * The inkaur mascot: an original ink-drop ghost (16x16 pixels), stored as a
 * pixel map in the binary (no image file) and drawn with half-block
 * characters, two pixel rows per terminal line.
 *
 * Layout follows the terminal width: the text (title in bold, then lines,
 * word-wrapped with a hanging indent for "- " items) goes beside the art
 * when at least 40 columns are left, below it on narrower terminals, and
 * alone when even the art does not fit. Without color (pipe, NO_COLOR,
 * --color never) only the plain, unwrapped text is printed, so scripts that
 * parse --version are not affected.
 */
void mascot_print(const char *title, const char *const *lines, size_t nlines);

#endif /* MASCOT_H */
