#pragma once

#include "Compiler/Diagnostic.h"
#include "Feature/CodeCompletion.h"
#include "Feature/SemanticToken.h"
#include "Protocol/Protocol.h"
#include "Support/FileSystem.h"
#include "Support/JSON.h"

namespace clice {

enum class PositionEncodingKind {
    UTF8,
    UTF16,
    UTF32,
};

struct PathMapping {
    std::string to_path(llvm::StringRef uri) {
        /// FIXME: Path mapping.
        return fs::toPath(uri);
    }

    std::string to_uri(llvm::StringRef path) {
        /// FIXME: Path mapping.
        return fs::toURI(path);
    }
};

/// @brief Iterates over Unicode codepoints in a UTF-8 encoded string and invokes a callback for
/// each codepoint.
///
/// Processes the input UTF-8 string, calculating the length of each Unicode codepoint in both
/// UTF-8 (bytes) and UTF-16 (code units), and passes these lengths to the callback.
/// Iteration stops early if the callback returns `false`.
///
/// ASCII characters are treated as 1-byte UTF-8 codepoints with a UTF-16 length of 1.
/// Non-ASCII characters are processed based on their leading byte to determine UTF-8 length:
/// - Valid lengths are 2 to 4 bytes.
/// - Astral codepoints (UTF-8 length of 4) have a UTF-16 length of 2 code units.
/// Invalid UTF-8 sequences are treated as single-byte ASCII characters.
///
/// Returns `false` if the callback stops the iteration.
template <typename Callback>
bool iterateCodepoints(llvm::StringRef content, const Callback& callback) {
    // Iterate over the input string, processing each codepoint.
    for(size_t index = 0; index < content.size();) {
        unsigned char c = static_cast<unsigned char>(content[index]);

        // Handle ASCII characters (1-byte UTF-8, 1-code-unit UTF-16).
        if(!(c & 0x80)) [[likely]] {
            if(!callback(1, 1)) {
                return true;
            }

            ++index;
            continue;
        }

        // Determine the length of the codepoint in UTF-8 by counting the leading 1s.
        size_t length = llvm::countl_one(c);

        // Validate UTF-8 encoding: length must be between 2 and 4.
        if(length < 2 || length > 4) [[unlikely]] {
            assert(false && "Invalid UTF-8 sequence");

            // Treat the byte as an ASCII character.
            if(!callback(1, 1)) {
                return true;
            }

            ++index;
            continue;
        }

        // Advance the index by the length of the current UTF-8 codepoint.
        index += length;

        // Calculate the UTF-16 length: astral codepoints (4-byte UTF-8) take 2 code units.
        if(!callback(length, length == 4 ? 2 : 1)) {
            return true;
        }
    }

    return false;
}

/// Remeasure the length (character count) of the content with the specified encoding kind.
inline std::uint32_t remeasure(llvm::StringRef content, PositionEncodingKind kind) {
    if(kind == PositionEncodingKind::UTF8) {
        return content.size();
    }

    if(kind == PositionEncodingKind::UTF16) {
        std::uint32_t length = 0;
        iterateCodepoints(content, [&](std::uint32_t, std::uint32_t utf16Length) {
            length += utf16Length;
            return true;
        });
        return length;
    }

    if(kind == PositionEncodingKind::UTF32) {
        std::uint32_t length = 0;
        iterateCodepoints(content, [&](std::uint32_t, std::uint32_t) {
            length += 1;
            return true;
        });
        return length;
    }

    std::unreachable();
}

class PositionConverter {
public:
    PositionConverter(llvm::StringRef content, PositionEncodingKind encoding) :
        content(content), encoding(encoding) {}

    /// Convert a offset to a proto::Position with given encoding.
    /// The input offset must be UTF-8 encoded and in order.
    proto::Position toPosition(uint32_t offset) {
        assert(offset <= content.size() && "Offset is out of range");
        assert(offset >= lastInput && "Offset must be in order");

        /// Fast path: return the last output.
        if(offset == lastInput) [[unlikely]] {
            return lastOutput;
        }

        /// The length of the current line.
        std::uint32_t lineLength = 0;

        /// Move the line offset to the current line.
        for(std::uint32_t i = lastLineOffset; i < offset; i++) {
            lineLength += 1;
            if(content[i] == '\n') {
                line += 1;
                lastLineOffset += lineLength;
                lineLength = 0;
            }
        }

        /// Get the content of the current line.
        auto lineContent = content.substr(lastLineOffset, lineLength);
        auto position = proto::Position{
            .line = line,
            .character = remeasure(lineContent, encoding),
        };

        /// Cache the result.
        lastInput = offset;
        lastOutput = position;

        return position;
    }

    template <typename Range, typename Proj = std::identity>
    void to_positions(Range&& range, const Proj&& proj = {}) {
        std::vector<uint32_t> offsets;
        for(auto&& item: range) {
            auto [begin, end] = proj(item);
            offsets.emplace_back(begin);
            offsets.emplace_back(end);
        }

        ranges::sort(offsets);

        for(auto&& offset: offsets) {
            if(auto it = cache.find(offset); it == cache.end()) {
                cache.try_emplace(offset, toPosition(offset));
            }
        }
    }

    proto::Position lookup(uint32_t offset) {
        auto it = cache.find(offset);
        assert(it != cache.end() && "Offset is not cached");
        return it->second;
    }

    proto::Range lookup(LocalSourceRange range) {
        auto it = cache.find(range.begin);
        assert(it != cache.end() && "Offset is not cached");
        auto begin = it->second;
        it = cache.find(range.end);
        assert(it != cache.end() && "Offset is not cached");
        auto end = it->second;
        return proto::Range{begin, end};
    }

private:
    std::uint32_t line = 0;
    /// The offset of the last line end.
    std::uint32_t lastLineOffset = 0;

    /// The input offset of last call.
    std::uint32_t lastInput = 0;
    proto::Position lastOutput = {0, 0};

    llvm::DenseMap<std::uint32_t, proto::Position> cache;

    llvm::StringRef content;
    PositionEncodingKind encoding;
};

inline std::uint32_t to_offset(clice::PositionEncodingKind kind,
                               llvm::StringRef content,
                               proto::Position position) {
    std::uint32_t offset = 0;
    for(auto i = 0; i < position.line; i++) {
        auto pos = content.find('\n');
        assert(pos != llvm::StringRef::npos && "Line value is out of range");

        offset += pos + 1;
        content = content.substr(pos + 1);
    }

    /// Drop the content after the line.
    content = content.take_until([](char c) { return c == '\n'; });
    assert(position.character <= content.size() && "Character value is out of range");

    if(position.character == 0) {
        return offset;
    }

    if(kind == PositionEncodingKind::UTF8) {
        offset += position.character;
        return offset;
    }

    if(kind == PositionEncodingKind::UTF16) {
        iterateCodepoints(content, [&](std::uint32_t utf8Length, std::uint32_t utf16Length) {
            assert(position.character >= utf16Length && "Character value is out of range");
            position.character -= utf16Length;
            offset += utf8Length;
            return position.character != 0;
        });
        return offset;
    }

    if(kind == PositionEncodingKind::UTF32) {
        iterateCodepoints(content, [&](std::uint32_t utf8Length, std::uint32_t) {
            assert(position.character >= 1 && "Character value is out of range");
            position.character -= 1;
            offset += utf8Length;
            return position.character != 0;
        });
        return offset;
    }

    std::unreachable();
}

}  // namespace clice

namespace clice::proto {

inline SymbolKind kind_map(clice::SymbolKind kind) {
    switch(kind.kind()) {
        case clice::SymbolKind::Comment: return SymbolKind::String;
        case clice::SymbolKind::Number: return SymbolKind::Number;
        case clice::SymbolKind::Character: return SymbolKind::String;
        case clice::SymbolKind::String: return SymbolKind::String;
        case clice::SymbolKind::Keyword: return SymbolKind::Variable;
        case clice::SymbolKind::Directive: return SymbolKind::Variable;
        case clice::SymbolKind::Header: return SymbolKind::String;
        case clice::SymbolKind::Module: return SymbolKind::Module;
        case clice::SymbolKind::Macro: return SymbolKind::Function;
        case clice::SymbolKind::MacroParameter: return SymbolKind::Variable;
        case clice::SymbolKind::Namespace: return SymbolKind::Namespace;
        case clice::SymbolKind::Class: return SymbolKind::Class;
        case clice::SymbolKind::Struct: return SymbolKind::Struct;
        case clice::SymbolKind::Union: return SymbolKind::Class;
        case clice::SymbolKind::Enum: return SymbolKind::Enum;
        case clice::SymbolKind::Type: return SymbolKind::TypeParameter;
        case clice::SymbolKind::Field: return SymbolKind::Field;
        case clice::SymbolKind::EnumMember: return SymbolKind::EnumMember;
        case clice::SymbolKind::Function: return SymbolKind::Function;
        case clice::SymbolKind::Method: return SymbolKind::Method;
        case clice::SymbolKind::Variable: return SymbolKind::Variable;
        case clice::SymbolKind::Parameter: return SymbolKind::Variable;
        case clice::SymbolKind::Label: return SymbolKind::Variable;
        case clice::SymbolKind::Concept: return SymbolKind::TypeParameter;
        case clice::SymbolKind::Attribute: return SymbolKind::Variable;
        case clice::SymbolKind::Operator:
        case clice::SymbolKind::Paren:
        case clice::SymbolKind::Bracket:
        case clice::SymbolKind::Brace:
        case clice::SymbolKind::Angle: return SymbolKind::Operator;
        case clice::SymbolKind::Conflict:
        case clice::SymbolKind::Invalid:
        default: return SymbolKind::Null;
    }
}

json::Value to_json(clice::PositionEncodingKind kind,
                    llvm::StringRef content,
                    llvm::ArrayRef<feature::SemanticToken> tokens);

json::Value to_json(clice::PositionEncodingKind kind,
                    llvm::StringRef content,
                    llvm::ArrayRef<feature::CompletionItem> items);

}  // namespace clice::proto
