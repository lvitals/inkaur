#ifndef VERCMP_H
#define VERCMP_H

/* Same ordering as pacman's vercmp(8) / alpm_pkg_vercmp():
 * returns <0, 0, >0 for a older/equal/newer than b ("[epoch:]ver[-rel]"). */
int pkg_vercmp(const char *a, const char *b);

#endif /* VERCMP_H */
