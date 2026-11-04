#include "git.h"
#include "alloc.h"
#include "config.h"
#include "proc.h"
#include "validate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const git_cfg[] = {
    "-c", "protocol.allow=never",
    "-c", "protocol.https.allow=always",
    "-c", "core.hooksPath=/dev/null",
    "-c", "core.symlinks=false",
    "-c", "core.fsmonitor=false",
    "-c", "core.attributesFile=/dev/null",
    "-c", "core.pager=cat",
    "-c", "transfer.fsckObjects=true",
    "-c", "fetch.fsckObjects=true",
    "-c", "submodule.recurse=false",
    "-c", "credential.helper=",
    "-c", "advice.detachedHead=false",
    "-c", "color.ui=never",
    "-c", "diff.external=",
    NULL
};

static const char *const git_env[] = {
    "GIT_CONFIG_NOSYSTEM=1",
    "GIT_CONFIG_GLOBAL=/dev/null",
    "GIT_TERMINAL_PROMPT=0",
    "GIT_PAGER=cat",
    "GIT_ASKPASS=/usr/bin/false",
    "GIT_PROTOCOL_FROM_USER=0",
    NULL
};

int git_run(const char *cwd, const char *const args[], StrBuf *capture)
{
    size_t na = 0, nc = 0;
    while (args[na])
        na++;
    while (git_cfg[nc])
        nc++;
    const char **argv = xcalloc(1 + nc + na + 1, sizeof(char *));
    size_t k = 0;
    argv[k++] = BIN_GIT;
    for (size_t i = 0; i < nc; i++)
        argv[k++] = git_cfg[i];
    for (size_t i = 0; i < na; i++)
        argv[k++] = args[i];
    argv[k] = NULL;

    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.cwd = cwd;
    o.capture = capture;
    o.stdin_null = true;
    o.extra_env = git_env;
    int rc = proc_run(argv, &o);
    free(argv);
    return rc;
}

int git_clone(const char *url, const char *dest)
{
    const char *a[] = { "clone", "--quiet", "--no-recurse-submodules",
                        "--origin", "origin", "--", url, dest, NULL };
    return git_run(NULL, a, NULL);
}

int git_fetch(const char *dir)
{
    const char *a[] = { "fetch", "--quiet", "--no-recurse-submodules",
                        "--prune", "origin", NULL };
    return git_run(dir, a, NULL);
}

int git_rev(const char *dir, const char *rev, char *out, size_t outsz)
{
    StrBuf b;
    sb_init(&b, 4096);
    char *spec = xasprintf("%s^{commit}", rev);
    const char *a[] = { "rev-parse", "--verify", "--quiet", "--end-of-options", spec, NULL };
    int rc = git_run(dir, a, &b);
    free(spec);
    if (rc == 0) {
        char *nl = strchr(b.data, '\n');
        if (nl)
            *nl = '\0';
        if (!valid_commit(b.data) || strlen(b.data) + 1 > outsz)
            rc = -1;
        else
            memcpy(out, b.data, strlen(b.data) + 1);
    }
    sb_free(&b);
    return rc == 0 ? 0 : -1;
}

int git_checkout_exact(const char *dir, const char *commit)
{
    if (!valid_commit(commit))
        return -1;
    const char *reset[] = { "reset", "--quiet", "--hard", commit, NULL };
    const char *clean[] = { "clean", "--quiet", "-ffdx", NULL };
    if (git_run(dir, reset, NULL) != 0 || git_run(dir, clean, NULL) != 0)
        return -1;
    char head[80];
    if (git_rev(dir, "HEAD", head, sizeof head) != 0 || strcmp(head, commit) != 0)
        return -1;
    return 0;
}

int git_show_file(const char *dir, const char *commit, const char *path, StrBuf *out)
{
    char *spec = xasprintf("%s:%s", commit, path);
    const char *a[] = { "cat-file", "blob", spec, NULL };
    int rc = git_run(dir, a, out);
    free(spec);
    return rc == 0 ? 0 : -1;
}

int git_diff(const char *dir, const char *from, const char *to, StrBuf *out)
{
    if (!valid_commit(from) || !valid_commit(to))
        return -1;
    const char *a[] = { "diff", "--no-color", "--no-ext-diff", "--no-textconv",
                        "--stat", "--patch", from, to, "--", NULL };
    return git_run(dir, a, out) == 0 ? 0 : -1;
}

int git_origin_url(const char *dir, StrBuf *out)
{
    const char *a[] = { "config", "--local", "--get", "remote.origin.url", NULL };
    int rc = git_run(dir, a, out);
    if (rc == 0) {
        char *nl = strchr(out->data, '\n');
        if (nl) {
            *nl = '\0';
            out->len = (size_t)(nl - out->data);
        }
    }
    return rc == 0 ? 0 : -1;
}
