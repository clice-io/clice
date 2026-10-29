module;

#define ROARING_EXCEPTIONS 0
#define ROARING_TERMINATE(message) std::abort()

module clice:support.bitmap;

import :support.prelude;

namespace clice {

using Bitmap = roaring::Roaring;

}  // namespace clice
