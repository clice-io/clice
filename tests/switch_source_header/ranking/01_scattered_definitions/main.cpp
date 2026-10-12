#include "text.h"

int process(char* text) {
    return trim(text) + split(text) + join(text) + upper(text);
}
