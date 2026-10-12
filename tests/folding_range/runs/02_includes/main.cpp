#include "alpha.h"
#include "beta.h"
#include "gamma.h"

#include "delta.h"

#include "alpha.h"
// a comment line ends the run
#include "beta.h"
#define HEADER "delta.h"
#include "gamma.h"
#include HEADER

#ifdef WINDOWS_BACKEND
#include "windows_api.h"
#include "windows_io.h"
#else
#include "posix_api.h"
#include "posix_io.h"
#endif
