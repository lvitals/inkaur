#ifndef RPC_H
#define RPC_H

#include "buf.h"
#include "json.h"

#include <stdbool.h>

/* One validated package record from the AUR RPC (v5). */
typedef struct {
    char *name;
    char *base;
    char *version;
    char *desc;        /* may be NULL */
    long outofdate;    /* 0 if not flagged */
    bool orphan;       /* Maintainer == null */
    long votes;        /* NumVotes */
    double popularity; /* Popularity */
    long first_submitted; /* unix time, 0 if unknown */
    long last_modified;   /* unix time, 0 if unknown */
    const JVal *raw;   /* borrowed from the response, for -Si */
} AurPkg;

typedef struct {
    JVal *root;
    AurPkg *pkgs;
    size_t n;
} RpcResult;

/* info for up to RPC_BATCH names. Invalid records are dropped with a warning. */
int rpc_info(const StrList *names, RpcResult *out);
/* search by=name-desc (by=provides when by_provides). */
int rpc_search(const char *term, bool by_provides, RpcResult *out);
void rpc_result_free(RpcResult *r);
const AurPkg *rpc_find(const RpcResult *r, const char *name);

#endif /* RPC_H */
