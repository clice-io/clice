#pragma once
#include "text.h"

// Taking npos's address makes the preamble instantiate its out-of-line
// definition, whose type is spelled through the class template.
inline const text::string::size_type* npos_address() {
    return &text::string::npos;
}
