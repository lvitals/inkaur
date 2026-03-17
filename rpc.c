#include "rpc.h"
#include "alloc.h"
#include "requests.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rpc_data *make_rpc_request(char *url)
{
        char *data = requests_get(url);
        if (data == NULL)
                return NULL;
        
        struct json *j = json_parse(data);
        if (j == NULL) {
                free(data);
                return NULL;
        }
        free(data);

        struct rpc_data *rpc_data = safe_malloc(sizeof(struct rpc_data));
        
        size_t resultcount = json_get_dict_number(j, "resultcount");
        struct json *results = json_get_dict_item(j, "results");
        char *error = NULL;

        char *result_type_str = json_get_dict_string(j, "type");
        rpc_result_t result_type = rpc_error;
        if (strcmp(result_type_str, "multiinfo") == 0)
                result_type = rpc_multiinfo;
        else if (strcmp(result_type_str, "search") == 0)
                result_type = rpc_search;
        else if (strcmp(result_type_str, "error") == 0) {
                result_type = rpc_error;
                error = safe_strdup(json_get_dict_string(j, "error"));
        }

        rpc_data->resultcount = resultcount;
        rpc_data->results = results;
        rpc_data->error = NULL;
        rpc_data->error = error;
        rpc_data->type = result_type;
        rpc_data->raw_json = j;

        return rpc_data;
}

void free_rpc_data(struct rpc_data *data)
{
        if (data == NULL)
                return;
        
        if (data->error != NULL)
                free(data->error);

        free_json_item(data->raw_json);
        free(data);
}

struct package *parse_package_json(struct json *j)
{
        if (j == NULL)
                return NULL;

        struct package *p = safe_malloc(sizeof(struct package));

        char *name = json_get_dict_string(j, "Name");
        char *desc = json_get_dict_string(j, "Description");
        char *version = json_get_dict_string(j, "Version");
        long outofdate = json_get_dict_number(j, "OutOfDate");

        p->name = safe_strdup(name);
        p->desc = safe_strdup(desc);
        p->version = safe_strdup(version);
        p->outofdate = outofdate;

        return p;
}

void free_package_data(struct package *p)
{
        if (p == NULL)
                return;

        free(p->name);
        free(p->desc);
        free(p->version);
        free(p);
}
