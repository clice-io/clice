// A path climbing out of the including file's directory links to the file
// it reaches, its target written without the `..`.
#include "../shared/config.h"

int limit = shared_limit;
