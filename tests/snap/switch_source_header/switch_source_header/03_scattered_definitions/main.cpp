/// # Definitions across several sources
///
/// - status: supported
/// - verify: server
/// - indexing: true
///
/// When several sources define a header's declarations, the one defining clearly the most is taken directly and the others stay listed

// switch: text.h
// switch: text_case.cpp

#include "text.h"

int process(char* text) {
    return trim(text) + split(text) + join(text) + upper(text);
}
