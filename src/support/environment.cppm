module;

#include "modules/prelude.h"

module clice:support.environment;

namespace clice {

/// The integer an environment variable holds: nullopt when it is unset or
/// holds no integer.
inline std::optional<std::uint64_t> env_integer(llvm::StringRef name) {
    auto value = llvm::sys::Process::GetEnv(name);
    std::uint64_t parsed = 0;
    if(!value || llvm::StringRef(*value).getAsInteger(10, parsed)) {
        return std::nullopt;
    }
    return parsed;
}

}  // namespace clice
