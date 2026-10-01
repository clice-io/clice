/// # Embedded and trailing includes
///
/// - status: supported
/// - diagnostics: expected
///
/// An include inside `extern "C"` or a type body, or one following the code, is no place for a new directive: it joins the includes at the top of the file

#include "support.h"

extern "C" {
#include "support.h"
}

enum Color {
#include "support.h"
};

using Strings = std::§(string)string;

#include "support.h"
