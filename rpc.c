#include "rpc.h"
#include "alloc.h"
#include "config.h"
#include "net.h"
#include "validate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rpc_call(const char *url, RpcResult *out)
{
    memset(out, 0, sizeof *out);
    StrBuf body;
    sb_init(&body, MAX_RPC_BYTES);
    if (http_get(url, &body) != 0) {
        sb_free(&body);
        return -1;
    }
    char err[128];
    JVal *root = json_parse(body.data, body.len, err, sizeof err);
    sb_free(&body);
    if (root == NULL) {
        fprintf(stderr, "error: invalid JSON from AUR: %s\n", err);
        return -1;
    }

    const char *type = json_get_str(root, "type");
    if (type == NULL) {
        fprintf(stderr, "error: malformed AUR response (no type)\n");
        json_free(root);
        return -1;
    }
    if (strcmp(type, "error") == 0) {
        fputs("error: AUR: ", stderr);
        safe_fputs(stderr, json_get_str(root, "error"));
        fputc('\n', stderr);
        json_free(root);
        return -1;
    }
    const JVal *results = json_get(root, "results");
    if (results == NULL || results->t != J_ARR) {
        fprintf(stderr, "error: malformed AUR response (no results)\n");
        json_free(root);
        return -1;
    }

    size_t n = json_len(results);
    out->root = root;
    out->pkgs = xcalloc(n ? n : 1, sizeof(AurPkg));
    for (size_t i = 0; i < n; i++) {
        const JVal *p = json_at(results, i);
        const char *name = json_get_str(p, "Name");
        const char *base = json_get_str(p, "PackageBase");
        const char *ver = json_get_str(p, "Version");
        if (!valid_pkgname(name) || !valid_pkgname(base) || !valid_version(ver)) {
            fputs("warning: ignoring AUR record with invalid name/base/version: ", stderr);
            safe_fputs(stderr, name ? name : "(null)");
            fputc('\n', stderr);
            continue;
        }
        AurPkg *a = &out->pkgs[out->n++];
        a->name = xstrdup(name);
        a->base = xstrdup(base);
        a->version = xstrdup(ver);
        a->desc = xstrdup(json_get_str(p, "Description"));
        double ood = 0;
        if (json_get_num(p, "OutOfDate", &ood) && ood > 0)
            a->outofdate = (long)ood;
        const JVal *m = json_get(p, "Maintainer");
        a->orphan = (m == NULL || m->t == J_NULL);
        double num = 0;
        if (json_get_num(p, "NumVotes", &num) && num >= 0 && num < 1e9)
            a->votes = (long)num;
        if (json_get_num(p, "Popularity", &num) && num >= 0 && num < 1e9)
            a->popularity = num;
        if (json_get_num(p, "FirstSubmitted", &num) && num > 0 && num < 1e11)
            a->first_submitted = (long)num;
        if (json_get_num(p, "LastModified", &num) && num > 0 && num < 1e11)
            a->last_modified = (long)num;
        a->raw = p;
    }
    return 0;
}

int rpc_info(const StrList *names, RpcResult *out)
{
    StrBuf url;
    sb_init(&url, 8192);
    sb_puts(&url, AUR_RPC_BASE "/info?");
    for (size_t i = 0; i < names->n; i++) {
        if (!valid_pkgname(names->v[i])) {
            fprintf(stderr, "error: invalid package name\n");
            sb_free(&url);
            return -1;
        }
        if (i)
            sb_putc(&url, '&');
        sb_puts(&url, "arg%5B%5D=");
        url_encode(&url, names->v[i]);
    }
    int r = rpc_call(url.data, out);
    sb_free(&url);
    return r;
}

int rpc_search(const char *term, bool by_provides, RpcResult *out)
{
    if (!valid_search_term(term)) {
        fprintf(stderr, "error: invalid search term (2-128 printable chars)\n");
        return -1;
    }
    StrBuf url;
    sb_init(&url, 4096);
    sb_puts(&url, AUR_RPC_BASE "/search/");
    url_encode(&url, term);
    sb_puts(&url, by_provides ? "?by=provides" : "?by=name-desc");
    int r = rpc_call(url.data, out);
    sb_free(&url);
    return r;
}

void rpc_result_free(RpcResult *r)
{
    for (size_t i = 0; i < r->n; i++) {
        free(r->pkgs[i].name);
        free(r->pkgs[i].base);
        free(r->pkgs[i].version);
        free(r->pkgs[i].desc);
    }
    free(r->pkgs);
    json_free(r->root);
    memset(r, 0, sizeof *r);
}

const AurPkg *rpc_find(const RpcResult *r, const char *name)
{
    for (size_t i = 0; i < r->n; i++)
        if (strcmp(r->pkgs[i].name, name) == 0)
            return &r->pkgs[i];
    return NULL;
}
