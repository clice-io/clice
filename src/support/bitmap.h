#pragma once

#include <cstdlib>

#define ROARING_EXCEPTIONS 0
#define ROARING_TERMINATE(message) std::abort()
#include "roaring/roaring.hh"

namespace clice {

using Bitmap = roaring::Roaring;

}  // namespace clice
