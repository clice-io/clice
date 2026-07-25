module;

#include <cstdlib>

#define ROARING_EXCEPTIONS 0
#define ROARING_TERMINATE(message) std::abort()
#include "roaring/roaring.hh"

export module clice.support:bitmap;

// Re-export the underlying type so consumers that name roaring::Roaring
// directly (index serialization) resolve it through the module.
export namespace roaring {

using ::roaring::Roaring;

}  // namespace roaring

export namespace clice {

using Bitmap = roaring::Roaring;

}  // namespace clice
