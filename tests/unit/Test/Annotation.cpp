#include "Test/Annotation.h"

namespace clice::testing {

AnnotatedSource AnnotatedSource::from(llvm::StringRef content) {
    std::string source;
    source.reserve(content.size());

    llvm::StringMap<std::uint32_t> offsets;
    llvm::StringMap<LocalSourceRange> ranges;
    std::vector<std::uint32_t> nameless_offsets;

    std::uint32_t offset = 0;
    std::uint32_t i = 0;

    // Helper lambda to parse a point annotation $(key).
    // It captures all necessary variables by reference.
    // Returns true if a point was successfully parsed, false otherwise.
    auto try_parse_point_annotation = [&]() -> bool {
        if(content[i] != '$') {
            return false;
        }

        // Peek ahead to see if it's "$(key)" or just "$"
        if(i + 1 < content.size() && content[i + 1] == '(') {
            // It's the full "$(key)" syntax
            uint32_t key_start = i + 2;
            size_t key_end = content.find(')', key_start);

            if(key_end == llvm::StringRef::npos) {
                return false;
            }  // Malformed

            llvm::StringRef key = content.slice(key_start, key_end);
            /// empty key is regarded as a nameless, and `()` is not consumed.
            if(key.empty()) {
                // It's the shorthand "$" syntax for an nameless key
                nameless_offsets.emplace_back(offset);
                i += 1;  // Advance cursor past the single '$'
            } else {
                offsets.try_emplace(key, offset);
                i = key_end + 1;  // Advance cursor past the entire "$(key)"
            }
            return true;
        } else {
            // It's the shorthand "$" syntax for an nameless key
            nameless_offsets.emplace_back(offset);
            i += 1;  // Advance cursor past the single '$'
            return true;
        }
    };

    while(i < content.size()) {
        // Check for a point annotation first.
        if(try_parse_point_annotation()) {
            continue;
        }

        char c = content[i];

        // Handle Range: @key[...]
        if(c == '@') {
            // Skip '@'
            i += 1;

            const char open_bracket = '[';
            const char close_bracket = ']';

            llvm::StringRef key = content.substr(i).take_until(
                [&](char c) { return isspace(c) || c == open_bracket; });
            i += key.size();

            while(i < content.size() && isspace(content[i])) {
                i++;
            }

            assert(i < content.size() && content[i] == open_bracket &&
                   "Expect @key[...] for ranges.");
            i += 1;  // Skip '['

            uint32_t begin_offset = offset;
            int bracket_level = 1;

            while(i < content.size() && bracket_level > 0) {
                // Inside a range, we can still have nested point annotations.
                if(try_parse_point_annotation()) {
                    continue;
                }

                char inner_c = content[i];
                if(inner_c == open_bracket)
                    bracket_level++;
                else if(inner_c == close_bracket)
                    bracket_level--;

                if(bracket_level > 0) {
                    source += inner_c;
                    offset += 1;
                    i += 1;
                } else {
                    i += 1;  // Skip the final ']'
                }
            }

            ranges.try_emplace(key, LocalSourceRange{begin_offset, offset});
            continue;
        }

        // If nothing else matched, it's a regular character.
        source += c;
        offset += 1;
        i += 1;
    }

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

    /// Save previous file to params.
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
