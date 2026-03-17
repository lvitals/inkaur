#include "../../json.h"
#include "../test_helper.h"
#include <string.h>

void test_json_parse_string() {
    char *json_str = "\"hello world\"";
    int idx;
    struct json *j = json_parse_string(json_str, &idx);
    
    ASSERT(j != NULL, "json_parse_string failed");
    ASSERT(j->type == json_string, "type should be string");
    ASSERT_STR_EQ(j->data.string, "hello world", "string content mismatch");
    ASSERT(idx == 13, "index should be 13");
    
    free_json_item(j);
}

void test_json_parse_number() {
    char *json_str = "123.45";
    int idx;
    struct json *j = json_parse_number(json_str, &idx);
    
    ASSERT(j != NULL, "json_parse_number failed");
    ASSERT(j->type == json_number, "type should be number");
    ASSERT(j->data.number == 123.45, "number value mismatch");
    
    free_json_item(j);
}

void test_json_parse_array() {
    char *json_str = "[\"a\", \"b\"]";
    struct json *j = json_parse(json_str);
    
    ASSERT(j != NULL, "json_parse failed for array");
    ASSERT(j->type == json_array, "type should be array");
    ASSERT(j->n_data_items == 2, "array size mismatch");
    
    struct json *item0 = json_get_array_item(j, 0);
    ASSERT(item0->type == json_string, "item 0 should be string");
    ASSERT_STR_EQ(item0->data.string, "a", "item 0 content mismatch");
    
    free_json_item(j);
}

void test_json_parse_dict() {
    char *json_str = "{\"key\": \"value\", \"num\": 123}";
    struct json *j = json_parse(json_str);
    ASSERT(j != NULL, "json_parse failed for dict");
    ASSERT(j->type == json_dict, "type should be dict");
    
    struct json *val = json_get_dict_item(j, "key");
    ASSERT(val != NULL, "failed to get dict item 'key'");
    ASSERT_STR_EQ(val->data.string, "value", "dict value mismatch");
    
    free_json_item(j);
}

void test_json_parse_bool_null(void) {
    struct json *j_true = json_parse("true");
    struct json *j_false = json_parse("false");
    struct json *j_null = json_parse("null");

    ASSERT(json_get_bool(j_true), "bool true mismatch");
    ASSERT(!json_get_bool(j_false), "bool false mismatch");
    ASSERT(j_null->type == json_null, "null parsing failed");

    free_json_item(j_true);
    free_json_item(j_false);
    free_json_item(j_null);
}

void test_json_safe_access() {
    struct json *j = json_parse("{\"results\": [{\"name\": \"inkaur\"}]}");
    struct json *res = json_safe_access(j, "%s %d %s", "results", 0, "name");
    ASSERT(res != NULL, "safe access failed");
    ASSERT_STR_EQ(res->data.string, "inkaur", "safe access content mismatch");
    free_json_item(j);
}

void test_json_nested() {
    char *json_str = "{\"a\": {\"b\": [1, 2, 3]}}";
    struct json *j = json_parse(json_str);
    ASSERT(j != NULL, "nested parse failed");
    struct json *b = json_access(j, "a", "b", NULL);
    ASSERT(b != NULL && b->type == json_array, "failed to access nested array");
    ASSERT(json_get_array_number(b, 1) == 2, "nested value mismatch");
    free_json_item(j);
}

void test_json_read_file() {
    // Test with a temporary file
    system("echo '{\"test\": 123}' > /tmp/test.json");
    char *content = json_read_file("/tmp/test.json");
    ASSERT(content != NULL, "json_read_file failed");
    struct json *j = json_parse(content);
    ASSERT(json_get_dict_number(j, "test") == 123, "file content parse mismatch");
    free(content);
    free_json_item(j);
}

void run_json_tests() {
    test_json_parse_string();
    test_json_parse_number();
    test_json_parse_array();
    test_json_parse_dict();
    test_json_parse_bool_null();
    test_json_safe_access();
    test_json_nested();
    test_json_read_file();
    printf("Unit tests for JSON passed!\n");
}
