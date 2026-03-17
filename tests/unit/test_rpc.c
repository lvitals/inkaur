#include "../../rpc.h"
#include "../../json.h"
#include "../test_helper.h"
#include <string.h>

void test_parse_package_json() {
    char *json_str = "{"
        "\"Name\": \"inkaur\","
        "\"Version\": \"3.0-1\","
        "\"Description\": \"Simple AUR helper\","
        "\"URL\": \"https://github.com/lvsantos/inkaur\","
        "\"NumVotes\": 10,"
        "\"Popularity\": 0.5,"
        "\"OutOfDate\": null"
    "}";
    
    struct json *j = json_parse(json_str);
    struct package *pkg = parse_package_json(j);
    
    ASSERT(pkg != NULL, "parse_package_json failed");
    ASSERT_STR_EQ(pkg->name, "inkaur", "pkg name mismatch");
    ASSERT_STR_EQ(pkg->version, "3.0-1", "pkg version mismatch");
    ASSERT(!pkg->outofdate, "pkg outofdate mismatch");
    
    free_package_data(pkg);
    free_json_item(j);
}

void run_rpc_tests() {
    test_parse_package_json();
    printf("Unit tests for RPC passed!\n");
}
