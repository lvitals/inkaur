#ifndef SEARCH_H
#define SEARCH_H

#include <stdbool.h>

#define MIN3(a, b, c) ((a) < (b) ? ((a) < (c) ? (a) : (c)) : \
                ((b) < (c) ? (b) : (c)))

int levenshtein(char *s1, char *s2);
int package_qsort_levenshtein(const void *one, const void *two);
void print_search_result(bool istty, char *name, 
                char *desc, char *ver, int ood, bool installed);
int search_aur(int n, char **terms);
void show_package_info(const char *package_name);

#endif  /* SEARCH_H */
