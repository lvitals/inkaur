#include "../test_helper.h"
#include "sha256.h"

#include <stdlib.h>
#include <unistd.h>

void run_sha256_tests(void);

static void hex_of(const char *s, size_t n, char out[SHA256_HEX_LEN + 1])
{
    Sha256 c;
    unsigned char d[32];
    sha256_init(&c);
    sha256_update(&c, s, n);
    sha256_final(&c, d);
    sha256_hex(d, out);
}

void run_sha256_tests(void)
{
    char h[SHA256_HEX_LEN + 1];
    /* FIPS 180-4 / NIST test vectors */
    hex_of("", 0, h);
    ASSERT_STR_EQ(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    hex_of("abc", 3, h);
    ASSERT_STR_EQ(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    hex_of(two, strlen(two), h);
    ASSERT_STR_EQ(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                  "448-bit message");

    /* one million 'a' fed in odd-sized chunks (exercises buffering) */
    Sha256 c;
    unsigned char d[32];
    char chunk[997];
    memset(chunk, 'a', sizeof chunk);
    sha256_init(&c);
    size_t left = 1000000;
    while (left) {
        size_t n = left < sizeof chunk ? left : sizeof chunk;
        sha256_update(&c, chunk, n);
        left -= n;
    }
    sha256_final(&c, d);
    sha256_hex(d, h);
    ASSERT_STR_EQ(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                  "one million a");

    /* copy + hash, then detect tampering of the copy */
    char dir[512];
    const char *t = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/inkaur-sha-XXXXXX", t ? t : "/tmp");
    ASSERT(mkdtemp(dir) != NULL, "mkdtemp");
    char src[600], dst[600], lnk[600];
    snprintf(src, sizeof src, "%s/pkg", dir);
    snprintf(dst, sizeof dst, "%s/staged", dir);
    snprintf(lnk, sizeof lnk, "%s/link", dir);
    FILE *f = fopen(src, "w");
    ASSERT(f != NULL, "create source");
    fputs("abc", f);
    fclose(f);

    char copied[SHA256_HEX_LEN + 1], again[SHA256_HEX_LEN + 1];
    ASSERT(sha256_copy(src, dst, copied) == 0, "copy succeeds");
    ASSERT_STR_EQ(copied, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                  "hash of the copied bytes");
    ASSERT(sha256_copy(src, dst, again) != 0, "copy never overwrites an existing file");
    ASSERT(sha256_file(dst, again) == 0 && strcmp(again, copied) == 0, "copy re-hashes the same");

    f = fopen(dst, "a");
    ASSERT(f != NULL, "tamper");
    fputs("x", f);
    fclose(f);
    ASSERT(sha256_file(dst, again) == 0 && strcmp(again, copied) != 0, "tampering is detected");

    ASSERT(symlink(src, lnk) == 0, "symlink");
    ASSERT(sha256_file(lnk, again) != 0, "symlinks are not followed");
    unlink(dst);
    ASSERT(sha256_copy(lnk, dst, again) != 0, "symlinked source refused");

    unlink(lnk);
    unlink(dst);
    unlink(src);
    rmdir(dir);
    printf("sha256: ok\n");
}
