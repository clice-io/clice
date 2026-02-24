#include "test/annotation.h"

#include <cctype>

namespace clice::testing {

AnnotatedSource AnnotatedSource::from(llvm::StringRef content) {
    std::string source;
    source.reserve(content.size());

    llvm::StringMap<std::uint32_t> offsets;
    llvm::StringMap<LocalSourceRange> ranges;
    std::vector<std::uint32_t> nameless_offsets;

    std::uint32_t offset = 0;
    std::uint32_t i = 0;

    auto try_parse_point_annotation = [&]() -> bool {
        if(content[i] != '$') {
            return false;
        }

        if(i + 1 < content.size() && content[i + 1] == '(') {
            std::uint32_t key_start = i + 2;
            std::size_t key_end = content.find(')', key_start);

            if(key_end == llvm::StringRef::npos) {
                return false;
            }

            llvm::StringRef key = content.slice(key_start, key_end);
            if(key.empty()) {
                nameless_offsets.emplace_back(offset);
                i += 1;
            } else {
                offsets.try_emplace(key, offset);
                i = key_end + 1;
            }
            return true;
        } else {
            nameless_offsets.emplace_back(offset);
            i += 1;
            return true;
        }
    };

    while(i < content.size()) {
        if(try_parse_point_annotation()) {
            continue;
        }

        char c = content[i];

        if(c == '@') {
            i += 1;

            const char open_bracket = '[';
            const char close_bracket = ']';

            llvm::StringRef key = content.substr(i).take_until(
                [&](char ch) { return isspace(ch) || ch == open_bracket; });
            i += key.size();

            while(i < content.size() && isspace(content[i])) {
                i++;
            }

            assert(i < content.size() && content[i] == open_bracket &&
                   "Expect @key[...] for ranges.");
            i += 1;

            std::uint32_t begin_offset = offset;
            int bracket_level = 1;

            while(i < content.size() && bracket_level > 0) {
                if(try_parse_point_annotation()) {
                    continue;
                }

                char inner_c = content[i];
                if(inner_c == open_bracket) {
                    bracket_level++;
                } else if(inner_c == close_bracket) {
                    bracket_level--;
                }

                if(bracket_level > 0) {
                    source += inner_c;
                    offset += 1;
                    i += 1;
                } else {
                    i += 1;
                }
            }

            ranges.try_emplace(key, LocalSourceRange{begin_offset, offset});
            continue;
        }

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
