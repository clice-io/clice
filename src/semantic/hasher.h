#pragma once

#include <cstdint>
#include <string>

#include "llvm/ADT/StringRef.h"

namespace clice {

/// The byte layout the identity and content hashes share: a tag or scheme
/// byte as itself, every integer as eight little-endian bytes, every
/// string with its length in front. Each scheme hashes the finished bytes
/// with xxh3 at the width it needs.
class ByteHasher {
public:
    void add_byte(std::uint8_t value) {
        buffer.push_back(static_cast<char>(value));
    }

    void add(std::uint64_t value) {
        for(unsigned i = 0; i < 8; i += 1) {
            buffer.push_back(static_cast<char>(value >> (8 * i)));
        }
    }

    void add(llvm::StringRef text) {
        add(static_cast<std::uint64_t>(text.size()));
        buffer.append(text.begin(), text.end());
    }

    llvm::StringRef bytes() const {
        return buffer;
    }

private:
    std::string buffer;
};

}  // namespace clice
