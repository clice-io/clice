#include "test/annotation.h"

#include <cctype>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

namespace clice::testing {

/// The annotation grammar reserves no ASCII character: the sigil is `§`
/// (U+00A7) and range bodies are delimited by `⟦` / `⟧` (U+27E6 / U+27E7),
/// none of which occur in real C++, Doxygen, Markdown, CMake or LSP
/// snippet content. ASCII parentheses are safe for names because they only
/// have meaning directly after the sigil:
///
///   §              nameless point       §(name)          named point
///   §⟦...⟧         nameless range       §(name)⟦...⟧     named range
///
/// Ranges may nest. Everything else — Doxygen tags, array subscripts,
/// `${1:...}` snippet placeholders — is ordinary source text and passes
/// through untouched. A stray `⟦` or `⟧` is a hard error so that typos
/// fail loudly instead of silently shifting offsets.
AnnotatedSource AnnotatedSource::from(llvm::StringRef content) {
    std::string source;
    source.reserve(content.size());

    llvm::StringMap<std::uint32_t> offsets;
    llvm::StringMap<LocalSourceRange> ranges;
    std::vector<std::uint32_t> nameless_offsets;

    constexpr llvm::StringRef sigil = "§";
    constexpr llvm::StringRef open_mark = "⟦";
    constexpr llvm::StringRef close_mark = "⟧";

    struct OpenSpan {
        llvm::StringRef key;
        std::uint32_t begin;
    };

    llvm::SmallVector<OpenSpan, 4> stack;

    std::uint32_t offset = 0;
    std::uint32_t i = 0;

    while(i < content.size()) {
        llvm::StringRef rest = content.substr(i);

        if(rest.starts_with(sigil)) {
            i += sigil.size();

            llvm::StringRef key;
            if(i < content.size() && content[i] == '(') {
                std::size_t key_end = content.find(')', i + 1);
                assert(key_end != llvm::StringRef::npos && "Unterminated §(name).");
                key = content.slice(i + 1, key_end);
                // A name must be a plain identifier (or empty): `§` directly
                // before a real parenthesized expression is ambiguous — write
                // the explicit nameless form `§()` in front of it instead.
                assert(llvm::all_of(key,
                                    [](char c) {
                                        return std::isalnum(static_cast<unsigned char>(c)) ||
                                               c == '_';
                                    }) &&
                       "§(name) requires an identifier name; use §() for a nameless point.");
                i = static_cast<std::uint32_t>(key_end) + 1;
            }

            if(content.substr(i).starts_with(open_mark)) {
                stack.push_back({key, offset});
                i += open_mark.size();
            } else if(key.empty()) {
                nameless_offsets.emplace_back(offset);
            } else {
                offsets.try_emplace(key, offset);
            }
            continue;
        }

        if(rest.starts_with(close_mark)) {
            assert(!stack.empty() && "`⟧` without a matching `§⟦`.");
            OpenSpan span = stack.pop_back_val();
            ranges.try_emplace(span.key, LocalSourceRange{span.begin, offset});
            i += close_mark.size();
            continue;
        }

        assert(!rest.starts_with(open_mark) && "`⟦` must follow `§` or `§(name)`.");

        source += content[i];
        offset += 1;
        i += 1;
    }

    assert(stack.empty() && "Unclosed `§⟦` annotation at end of input.");

    return AnnotatedSource{
        std::move(source),
        std::move(offsets),
        std::move(ranges),
        std::move(nameless_offsets),
    };
}

void AnnotatedSources::add_sources(llvm::StringRef content) {
    std::string curr_file;
    std::string curr_content;

    auto save_previous_file = [&]() {
        if(curr_file.empty()) {
            return;
        }

        add_source(curr_file, curr_content);
        curr_file.clear();
        curr_content.clear();
    };

    while(!content.empty()) {
        llvm::StringRef line = content.take_front(content.find_first_of("\r\n"));
        content = content.drop_front(line.size());
        if(content.starts_with("\r\n")) {
            content = content.drop_front(2);
        } else if(content.starts_with("\n")) {
            content = content.drop_front(1);
        }

        if(line.starts_with("#[") && line.ends_with("]")) {
            save_previous_file();
            curr_file = line.slice(2, line.size() - 1).str();
        } else if(!curr_file.empty()) {
            curr_content += line;
            curr_content += '\n';
        }
    }

    save_previous_file();
}

}  // namespace clice::testing
