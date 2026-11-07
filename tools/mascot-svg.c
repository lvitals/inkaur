/* Writes mascot.svg from the pixel map in mascot.c (make mascot.svg). */
#include "mascot.h"

#include <stdio.h>

int main(void)
{
    mascot_write_svg(stdout, 10);
    return ferror(stdout) ? 1 : 0;
}
