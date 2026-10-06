module;

#include "modules/prelude.h"

#define ROARING_EXCEPTIONS 0
#define ROARING_TERMINATE(message) std::abort()

module clice:support.bitmap;

namespace clice {

using Bitmap = roaring::Roaring;

}  // namespace clice
