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
        memset(p, 0, sizeof(struct package));

        struct json *name_j = json_get_dict_item(j, "Name");
        struct json *desc_j = json_get_dict_item(j, "Description");
        struct json *version_j = json_get_dict_item(j, "Version");
        struct json *outofdate_j = json_get_dict_item(j, "OutOfDate");

        if (name_j && name_j->type == json_string)
                p->name = safe_strdup(name_j->data.string);
        if (desc_j && desc_j->type == json_string)
                p->desc = safe_strdup(desc_j->data.string);
        if (version_j && version_j->type == json_string)
                p->version = safe_strdup(version_j->data.string);
        if (outofdate_j && outofdate_j->type == json_number)
                p->outofdate = (long)outofdate_j->data.number;

        struct json *depends_j = json_get_dict_item(j, "Depends");
        if (depends_j && depends_j->type == json_array) {
                p->depends_count = json_get_size(depends_j);
                p->depends = safe_malloc(sizeof(char *) * p->depends_count);
                for (size_t i = 0; i < p->depends_count; i++) {
                        p->depends[i] = safe_strdup(json_get_array_string(depends_j, i));
                }
        }

        struct json *makedepends_j = json_get_dict_item(j, "MakeDepends");
        if (makedepends_j && makedepends_j->type == json_array) {
                p->makedepends_count = json_get_size(makedepends_j);
                p->makedepends = safe_malloc(sizeof(char *) * p->makedepends_count);
                for (size_t i = 0; i < p->makedepends_count; i++) {
                        p->makedepends[i] = safe_strdup(json_get_array_string(makedepends_j, i));
                }
        }

        return p;
}

void free_package_data(struct package *p)
{
        if (p == NULL)
                return;

        free(p->name);
        free(p->desc);
        free(p->version);

        for (size_t i = 0; i < p->depends_count; i++)
                free(p->depends[i]);
        free(p->depends);

        for (size_t i = 0; i < p->makedepends_count; i++)
                free(p->makedepends[i]);
        free(p->makedepends);

        free(p);
}
