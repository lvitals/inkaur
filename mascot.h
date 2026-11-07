#ifndef MASCOT_H
#define MASCOT_H

#include <stddef.h>

/*
 * The inkaur mascot: an original ink-drop ghost, stored as a pixel map in
 * the binary (no image file) and drawn with half-block characters, two pixel
 * rows per terminal line. lines[] is printed to its right, vertically
 * centered. Without color (pipe, NO_COLOR, --color never) only the text is
 * printed, so scripts that parse --version are not affected.
 */
void mascot_print(const char *const *lines, size_t nlines);

#endif /* MASCOT_H */
