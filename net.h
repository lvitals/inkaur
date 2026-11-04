#ifndef NET_H
#define NET_H

#include "buf.h"

/*
 * HTTPS GET through the system curl binary (no libcurl linkage).
 * curl is a hard dependency of pacman, so it is always present on Arch.
 * Hardening: ~/.curlrc ignored (-q), https only (also on redirects),
 * TLS >= 1.2, URL globbing off, response size and time bounded.
 */
int http_get(const char *url, StrBuf *out);

/* Percent-encode everything except RFC 3986 unreserved characters. */
void url_encode(StrBuf *out, const char *s);

#endif /* NET_H */
