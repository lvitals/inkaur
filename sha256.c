#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(Sha256 *c, const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
    c->h[5] += f;
    c->h[6] += g;
    c->h[7] += h;
}

void sha256_init(Sha256 *c)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->nbuf = 0;
}

void sha256_update(Sha256 *c, const void *data, size_t n)
{
    const unsigned char *p = data;
    c->len += n;
    if (c->nbuf) {
        size_t take = 64 - c->nbuf < n ? 64 - c->nbuf : n;
        memcpy(c->buf + c->nbuf, p, take);
        c->nbuf += take;
        p += take;
        n -= take;
        if (c->nbuf < 64)
            return;
        block(c, c->buf);
        c->nbuf = 0;
    }
    while (n >= 64) {
        block(c, p);
        p += 64;
        n -= 64;
    }
    memcpy(c->buf, p, n);
    c->nbuf = n;
}

void sha256_final(Sha256 *c, unsigned char out[32])
{
    uint64_t bits = c->len * 8;
    unsigned char pad[72];
    size_t padn = (c->nbuf < 56 ? 56 : 120) - c->nbuf;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (int i = 0; i < 8; i++)
        pad[padn + (size_t)i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(c, pad, padn + 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (unsigned char)(c->h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(c->h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(c->h[i] >> 8);
        out[4 * i + 3] = (unsigned char)c->h[i];
    }
    memset(c, 0, sizeof *c);
}

void sha256_hex(const unsigned char d[32], char hex[SHA256_HEX_LEN + 1])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[2 * i] = digits[d[i] >> 4];
        hex[2 * i + 1] = digits[d[i] & 15];
    }
    hex[SHA256_HEX_LEN] = '\0';
}

static int open_regular(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int write_all(int fd, const unsigned char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Hash fd (and copy it to out_fd when >= 0). */
static int hash_fd(int fd, int out_fd, char hex[SHA256_HEX_LEN + 1])
{
    Sha256 c;
    sha256_init(&c);
    unsigned char buf[65536];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        sha256_update(&c, buf, (size_t)r);
        if (out_fd >= 0 && write_all(out_fd, buf, (size_t)r) != 0)
            return -1;
    }
    unsigned char d[32];
    sha256_final(&c, d);
    sha256_hex(d, hex);
    return 0;
}

int sha256_file(const char *path, char hex[SHA256_HEX_LEN + 1])
{
    int fd = open_regular(path);
    if (fd < 0)
        return -1;
    int rc = hash_fd(fd, -1, hex);
    close(fd);
    return rc;
}

int sha256_copy(const char *src, const char *dst, char hex[SHA256_HEX_LEN + 1])
{
    int in = open_regular(src);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) {
        close(in);
        return -1;
    }
    int rc = hash_fd(in, out, hex);
    if (rc == 0 && fsync(out) != 0)
        rc = -1;
    if (close(out) != 0)
        rc = -1;
    close(in);
    if (rc != 0)
        unlink(dst);
    return rc;
}
