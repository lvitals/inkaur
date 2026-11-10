#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

/* SHA-256 (FIPS 180-4), no third-party code. */
typedef struct {
    uint32_t h[8];
    uint64_t len;          /* bytes hashed so far */
    unsigned char buf[64];
    size_t nbuf;
} Sha256;

#define SHA256_HEX_LEN 64

void sha256_init(Sha256 *c);
void sha256_update(Sha256 *c, const void *data, size_t n);
void sha256_final(Sha256 *c, unsigned char out[32]);
/* Lowercase hex of a 32-byte digest into hex (at least 65 bytes). */
void sha256_hex(const unsigned char d[32], char hex[SHA256_HEX_LEN + 1]);

/* Hash a regular file opened with O_NOFOLLOW. 0 on success. */
int sha256_file(const char *path, char hex[SHA256_HEX_LEN + 1]);

/*
 * Copy a regular file (opened with O_NOFOLLOW, never a symlink) to dst,
 * created with O_EXCL and mode 0600, hashing exactly the bytes written.
 * The copy, not the original, is what gets audited and installed, so a file
 * swapped in the build directory afterwards cannot reach pacman.
 */
int sha256_copy(const char *src, const char *dst, char hex[SHA256_HEX_LEN + 1]);

#endif /* SHA256_H */
