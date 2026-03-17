#ifndef RPC_H
#define RPC_H

#include "json.h"

typedef enum {
        rpc_search,
        rpc_multiinfo,
        rpc_error,
} rpc_result_t;

struct rpc_data {
        size_t resultcount;
        rpc_result_t type;
        char *error;
        struct json *results;
        struct json *raw_json;
};

struct package {
        char *name;
        char *desc;
        char *version;
        long outofdate;
};

struct rpc_data *make_rpc_request(char *url);
void free_rpc_data(struct rpc_data *data);
struct package *parse_package_json(struct json *j);
void free_package_data(struct package *p);

#endif  /* RPC_H */
