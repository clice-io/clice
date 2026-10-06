module;

#include "modules/prelude.h"

module clice:support.json;

namespace clice {

/// JSON persisting paths. A path that is not UTF-8 is written with U+FFFD for
/// its bad bytes, as JSON holds only UTF-8: it then names no file, and only
/// what it describes is rebuilt, where a blob that cannot be written would
/// lose everything.
struct PathJsonConfig {
    constexpr static auto invalid_utf8 = kota::codec::invalid_utf8::Replace;
};

}  // namespace clice
