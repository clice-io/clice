#include "../include/parser.h"

int parse_twice(const char* text) {
    return parse(text) + parse(text + 1);
}
