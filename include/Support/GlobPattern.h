#pragma once

#include <expected>
#include <string>
#include <bitset>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

using GlobCharSet = std::bitset<256>;

/// This class implements a glob pattern matcher to parse patterns to
/// watch relative to the base path.
///
/// Glob patterns can have the following syntax:
/// - `*` to match one or more characters in a path segment
/// - `?` to match on one character in a path segment
/// - `**` to match any number of path segments, including none
/// - `{}` to group conditions (e.g. `***.{ts,js}` matches all TypeScript
///   and JavaScript files)
/// - `[]` to declare a range of characters to match in a path segment
///   (e.g., `example.[0-9]` to match on `example.0`, `example.1`, …)
/// - `[!...]` to negate a range of characters to match in a path segment
///   (e.g., `example.[!0-9]` to match on `example.a`, `example.b`,
///   but not `example.0`)
///
///   Note: Use only `/` for path segment seperator
class GlobPattern {
public:
    /// \param Pat the pattern to match against
    /// \param MaxSubPatterns if provided limit the number of allowed subpatterns
    ///                       created from expanding braces otherwise disable
    ///                       brace expansion
    static std::expected<GlobPattern, std::string> create(llvm::StringRef s,
                                                          size_t max_subpattern_num = 100);

    // Returns true for glob pattern "*" or "**". Can be used to avoid expensive
    // preparation/acquisition of the input for match().
    bool isTrivialMatchAll() const {
        if(!prefix.empty()) {
            // have no prefix
            return false;
        }
        if((sub_globs.size() == 1 && sub_globs[0].getPat() == "*") ||
           (sub_globs.size() == 2 && sub_globs[0].getPat() == "**")) {
            // not "*" or "**"
            return true;
        }
        // default return false
        return false;
    }

    /// \returns \p true if \p str matches this glob pattern
    bool match(llvm::StringRef s);

private:
    /// GlobPattern is seperated into `Prefix + SubGlobPattern`
    std::string prefix;

    /// if prefix contains full path:
    ///
    /// xxx/yyy/*.c
    /// ~~~~~~^
    ///
    /// prefix contains incomplete path:
    /// xxx/yyy*.c
    /// ~~~~~~^
    ///
    bool prefix_at_seg_end = false;

    /// SubGlobPattern:
    /// Pattern `foo.{c,cpp,cppm}`
    /// -> extend to 3 SubGlobPatterns: `foo.c`, `foo.cpp`, `foo.cppm`
    struct SubGlobPattern {
        /// \param Pat the pattern to match against
        static std::expected<SubGlobPattern, std::string> create(llvm::StringRef s);
        /// \returns \p true if \p S matches this glob pattern
        bool match(llvm::StringRef str) const;

        llvm::StringRef getPat() const {
            return llvm::StringRef{pat.data(), pat.size()};
        }

        struct Bracket {
            size_t next_offset;
            GlobCharSet bytes;
        };

        llvm::SmallVector<Bracket, 0> brackets;

        // GlobSegment devide patterm into segments by '/'
        // SubPattern:
        // **include/aaa/bbb/test[0-9].cc
        // ^~~~~~~~~^~~~^~~~^~~~~~~~~~~~~^
        //     1      2   3        4
        // Devided into 4 segments
        //
        // SubPattern:
        // **/include/aaa/bbb/test[0-9].h
        // ^~^~~~~~~~^~~~^~~~^~~~~~~~~~~~^
        //  1    2     3   4       5
        //  Devided into 5 segments
        struct GlobSegment {
            size_t start;
            size_t end;
        };

        llvm::SmallVector<GlobSegment, 6> glob_segments;

        llvm::SmallVector<char, 0> pat;
    };

    llvm::SmallVector<SubGlobPattern, 1> sub_globs;
};

}  // namespace clice
