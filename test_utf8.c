/*
 * test that json_object_to_json_string returns correct \u escaped unicode
 * run with make test6 && ./test6
*/

#include <stdio.h>
#include <string.h>
#include "config.h"

#include "json_inttypes.h"
#include "json_object.h"
#include "json_tokener.h"

void print_hex( const unsigned char* s) {
        const unsigned char *iter = s;
        unsigned char ch;
        while ((ch = *iter++) != 0) {
           if( ',' != ch)
            printf("%x ", ch);
           else
            printf( ",");
        }
        printf("\n");
}

int main() {
    const char *input = "\"I\\u00f1t\\u00ebrn\\u00e2ti\\u00f4n\\ufffdliz\\u00e6ti\\u00f8n\\u0000\"";
    // const char *correct_unicode_byte_string = "I\xf1t\xebrn\xe2ti\xf4n\ufffdliz\xe6ti\xf8n\x00";
    const char *correct_utf8_byte_string = "I\xc3\xb1t\xc3\xabrn\xc3\xa2ti\xc3\xb4n\xef\xbf\xbdliz\xc3\xa6ti\xc3\xb8n\x00";
    struct json_object *parse_result = json_tokener_parse((char*)input);
    const char *utf8 = json_object_get_string(parse_result);
    const char *output = json_object_to_json_string(parse_result);
    printf("input: %s\n", input);
    printf("output: %s\n", output);
    printf("utf8: %s\n", utf8);
    // as a double check re-parse the encoded string
    // struct json_object *parse_result2 = json_tokener_parse((char*)output);
    // const char *output2 = json_object_to_json_string(parse_result2);
    // output2 will help verify that parsing and encoding ends up at the same thing
    // printf("output: %s\n", output2);

    int strings_match = !strcmp( input, output);
    if (!strings_match) {
        printf("JSON parse result doesn't match expected string\n");
        printf("expected string bytes: ");
        print_hex((const unsigned char *) input);
        printf("output string bytes:   ");
        print_hex((const unsigned char *) output);
        printf("FAIL\n");
        return(1);
    }
    strings_match = !strcmp( utf8, correct_utf8_byte_string);
    if (!strings_match) {
        printf("correct utf8: %s\n", correct_utf8_byte_string);
        printf("utf8 strings don't match\n");
        printf("expected string bytes: ");
        print_hex((const unsigned char *) correct_utf8_byte_string);
        printf("output string bytes:   ");
        print_hex((const unsigned char *) utf8);
        printf("FAIL\n");
        return(1);
    }
    json_object_put(parse_result);
    printf("PASS\n");
    return(0);
}
