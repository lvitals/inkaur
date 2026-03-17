#ifndef REQUESTS_H
#define REQUESTS_H

#include <stdlib.h>

struct curl_str {
        char *ptr;
        size_t len;
};

void init_string(struct curl_str *s);
size_t write_func(void *ptr, size_t size, size_t nmemb, struct curl_str *s);
char *requests_get(char *url);

#endif  /* REQUESTS_H */
