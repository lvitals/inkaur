#include "net.h"
#include "config.h"
#include "proc.h"

#include <stdio.h>
#include <string.h>

void url_encode(StrBuf *out, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
            c == '~') {
            sb_putc(out, (char)c);
        } else {
            sb_putc(out, '%');
            sb_putc(out, hex[c >> 4]);
            sb_putc(out, hex[c & 15]);
        }
    }
}

int http_get(const char *url, StrBuf *out)
{
    if (strncmp(url, "https://", 8) != 0) {
        fprintf(stderr, "error: refusing non-https URL\n");
        return -1;
    }
    char maxsize[32];
    snprintf(maxsize, sizeof maxsize, "%u", (unsigned)MAX_RPC_BYTES);

    const char *argv[] = {
        BIN_CURL,
        "-q",                       /* must be first: ignore ~/.curlrc */
        "--silent", "--show-error", "--fail",
        "--globoff",
        "--proto", "=https",
        "--proto-redir", "=https",
        "--tlsv1.2",
        "--max-redirs", "3",
        "--connect-timeout", "15",
        "--max-time", "60",
        "--max-filesize", maxsize,
        "--user-agent", "inkaur/" INKAUR_VERSION,
        "--header", "Accept: application/json",
        "--url", url,
        NULL
    };
    out->max = MAX_RPC_BYTES;
    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.capture = out;
    o.stdin_null = true;
    int rc = proc_run(argv, &o);
    if (rc != 0) {
        fprintf(stderr, "error: request to the AUR failed (curl exit %d)\n", rc);
        return -1;
    }
    return 0;
}
