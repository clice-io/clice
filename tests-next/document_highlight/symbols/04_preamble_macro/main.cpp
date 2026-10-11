// A macro defined among the leading directives is compiled into the
// preamble: the server's highlights join its definition from there with
// the uses in the rest of the file.

#include "config.h"
#define §(macro_def)RETRIES 3

int attempts() {
    return §(macro_use)RETRIES + configured() * RETRIES;
}
