module clice:support.environment;

import :support.prelude;

namespace clice {

/// The integer an environment variable holds: nullopt when it is unset or
/// holds no integer.
inline std::optional<std::uint64_t> env_integer(llvm::StringRef name) {
    std::string value;
    std::uint64_t parsed = 0;
    if(!sys::get_env(value, name) || llvm::StringRef(value).getAsInteger(10, parsed)) {
        return std::nullopt;
    }
    return parsed;
}

/// Test hook: CLICE_TEST_JITTER=<ms> makes this a random delay of up to that
/// many milliseconds, zero while it is unset. The places that race with an
/// editor's actions (a worker starting a request's work, a background index
/// round starting) wait it out, so a run of the cases under it meets the
/// orders a loaded machine meets; a case whose answer changes left an order
/// unpinned.
inline chrono::milliseconds test_jitter() {
    const static auto max = env_integer("CLICE_TEST_JITTER");
    if(!max) {
        return chrono::milliseconds(0);
    }
    thread_local std::minstd_rand random(std::random_device{}());
    return chrono::milliseconds(std::uniform_int_distribution<std::uint64_t>(0, *max)(random));
}

}  // namespace clice
