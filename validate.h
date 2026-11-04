#ifndef VALIDATE_H
#define VALIDATE_H

#include "buf.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Input validation. Every string that ends up in a URL, a filesystem path or
 * the argv of a child process must pass one of these first.
 */

/* Package name as accepted by makepkg: [A-Za-z0-9@._+-], not starting with
 * '-' or '.', 1..MAX_NAME_LEN bytes. Rejects ".", "..", "/" and so on. */
bool valid_pkgname(const char *s);

/* pkgver/pkgrel/epoch characters: [A-Za-z0-9._+:~-], no leading '-'. */
bool valid_version(const char *s);

/* Dependency string "name[<op>version]". On success copies the bare name
 * into name_out (size >= MAX_NAME_LEN + 1). */
bool valid_depstring(const char *s, char *name_out, size_t name_sz);

/* Full OpenPGP fingerprint: exactly 40 (v4) or 64 (v5/v6) hex digits.
 * Short/long key IDs are rejected on purpose (collisions are practical). */
bool valid_fingerprint(const char *s);

/* git object id: 40 (sha1) or 64 (sha256) lowercase hex digits. */
bool valid_commit(const char *s);

/* Search keyword: printable, 2..128 bytes, no control characters. */
bool valid_search_term(const char *s);

/* Decode one UTF-8 sequence; returns its length (1..4) or 0 if invalid. */
size_t utf8_decode(const unsigned char *s, size_t n, uint32_t *cp);

/* Code points that can hide or reorder code on screen (Trojan Source:
 * bidi overrides, zero-width characters, BOM). */
bool is_deceptive_codepoint(uint32_t cp);

/*
 * Make arbitrary bytes safe to print on a terminal: control characters
 * become ^X, invalid UTF-8 becomes \xNN and deceptive code points become
 * <U+XXXX>. With keep_newlines, '\n' and '\t' pass through unchanged.
 */
void sanitize_append(StrBuf *out, const char *s, size_t n, bool keep_newlines);
void safe_fputs(FILE *fp, const char *s);   /* single-line sanitized */

#endif /* VALIDATE_H */
