
#ifndef OUTPUT_H
#define OUTPUT_H

#include <stdbool.h>

#define BOLD "\033[1m"
#define RED "\033[91m"
#define GREEN "\033[92m"
#define BLUE "\033[94m"
#define ENDC "\033[0m"

void indent_print(char *str, int indent);
bool stdout_is_tty(void);
void print_diff(char *a, char *b);

#endif
