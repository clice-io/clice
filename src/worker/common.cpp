module;

#include "modules/prelude.h"

module clice;

import :support.environment;
import :worker.common;

namespace clice {

std::size_t max_index_bytes() {
    static std::size_t limit = env_integer("CLICE_TEST_MAX_INDEX_BYTES").value_or(56 * 1024 * 1024);
    return limit;
}

}  // namespace clice
