module clice:support.bytes;

import :support.prelude;

namespace clice {

/// Bytes as the APIs taking chars hold them: LLVM's, LMDB's, the files'.
inline llvm::StringRef as_chars(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// The bytes an API handing out chars holds.
inline std::span<const std::byte> as_bytes(llvm::StringRef chars) {
    return std::as_bytes(std::span(chars.data(), chars.size()));
}

/// A copy of the bytes an API handing out chars holds.
inline std::vector<std::byte> copy_bytes(llvm::StringRef chars) {
    auto bytes = as_bytes(chars);
    return {bytes.begin(), bytes.end()};
}

}  // namespace clice
