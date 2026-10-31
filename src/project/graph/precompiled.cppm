module clice:project.precompiled;

import :support.failure_record;
import :support.prelude;

namespace clice {

/// Length-prefix each part (so embedded NULs cannot create colliding
/// splits) and return the 32-hex xxh3_128bits cache key.
std::string cache_key(std::initializer_list<llvm::StringRef> parts);

/// The name in the store of a new build under `key`: the key and a nonce,
/// so a rebuild publishes beside the product it replaces, never over it.
std::string new_blob(llvm::StringRef key);

/// The record of a build that failed with `cause`: `death` is the identity
/// of the worker death it crashed, empty for errors in the user's code.
FailureRecord failed_build(llvm::StringRef death, std::string cause);

}  // namespace clice
