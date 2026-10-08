/// # Preprocessor directives
///
/// - status: supported
///
/// A directive is selected as a whole line, after the header name it includes
/// or the brackets of its macro body

#include "§(header)config.h"
#define SQUARE(x) ((§(body)x) * (x))

int area = SQUARE(SIDE);
