// An include inside `extern "C"` or an enum body, or one after the code, is no
// place for a new directive: it joins the includes at the top of the file.

#include "config.h"

extern "C" {
#include "c_api.h"
}

enum Color {
#include "colors.def"
};

using Strings = std::§(string)string;

#include "palette.tpp"
