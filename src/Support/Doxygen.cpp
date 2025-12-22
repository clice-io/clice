#include "Support/Doxygen.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/raw_ostream.h"

namespace clice {

void DoxygenInfo::add_block_command_comment(llvm::StringRef tag, llvm::StringRef content) {
    auto [it, _] = block_command_comments.try_emplace(tag);
    it->second.emplace_back(content.str());
}

void DoxygenInfo::add_param_command_comment(
    llvm::StringRef name,
    llvm::StringRef content,
    DoxygenInfo::ParamCommandCommentContent::ParamDirection direction) {
    auto [it, not_exist] = param_command_comments.try_emplace(name);
    if(not_exist) {
        it->second.content = content;
        it->second.direction = direction;
    } else {
        // Merge the info as doxygen does
        if(it->second.direction == ParamCommandCommentContent::ParamDirection::Unspecified &&
           direction != ParamCommandCommentContent::ParamDirection::Unspecified) {
            // Update the direction if not assigned
            it->second.direction = direction;
        }
        it->second.content += "\n";
        it->second.content += content;
    }
}

std::optional<DoxygenInfo::ParamCommandCommentContent*>
    DoxygenInfo::find_param_info(llvm::StringRef name) {
    if(auto it = param_command_comments.find_as(name); it != param_command_comments.end()) {
        return &it->getSecond();
    }
    return std::nullopt;
}

std::vector<std::pair<llvm::StringRef, llvm::ArrayRef<DoxygenInfo::BlockCommandCommentContent>>>
    DoxygenInfo::get_block_command_comments() {
    std::vector<std::pair<llvm::StringRef, llvm::ArrayRef<DoxygenInfo::BlockCommandCommentContent>>>
        res{};
    for(auto& [tag, content]: block_command_comments) {
        auto& pair = res.emplace_back();
        pair.first = tag;
        pair.second = content;
    }
    return res;
}

/// Process inline commands, we only interested in `\b` (bold), `\e` (italic) and `\c` (inline code)
///
/// \param line   The line
/// \param result Where should we output the result to
static void process_non_command_line(llvm::StringRef line, llvm::raw_ostream& result) {
    while(!line.empty()) {
        auto pos = line.find_first_of("\\@");
        if(pos == llvm::StringRef::npos || pos == line.size()) {
            result << line;
            break;
        }
        result << line.take_front(pos);
        line = line.drop_front(pos);
        if(line.size() <= 4) {
            // shorter than `@b x`
            result << line;
            break;
        }

        char opt = line[1];
        if(!llvm::isSpace(line[2])) {
            // Not an inline command, output as is
            result << line.take_front(2);
            line = line.drop_front(2);
            continue;
        }

        // Skip spaces
        size_t word_left = line.find_first_not_of(" \t\v\f\r", 2);
        if(word_left == llvm::StringRef::npos) {
            result << line;
            break;
        }

        word_left -= 2;
        // adjust relative to current line
        llvm::StringRef rest = line.drop_front(word_left + 2);
        size_t word_end = rest.find_first_of(" \t\v\f\r");
        if(word_end == llvm::StringRef::npos)
            word_end = rest.size();

        llvm::StringRef word = rest.take_front(word_end);
        line = rest.drop_front(word_end);

        if(word.empty()) {
            result << line;
            break;
        }

        switch(opt) {
            case 'b': result << "**" << word << "**"; break;
            case 'e': result << '*' << word << '*'; break;
            case 'c': result << '`' << word << '`'; break;
            default: result << '\\' << opt << ' ' << word; break;
        }
    }
    result << '\n';
}

/// Always returns the referense of next line after this paragragh
static void process_paragragh(llvm::SmallVector<llvm::StringRef>::iterator& line_ref,
                              const llvm::SmallVector<llvm::StringRef>::iterator& end,
                              DoxygenInfo& di,
                              llvm::raw_ostream& rest) {
    auto consume_command_block = [&line_ref, &end](llvm::raw_ostream& os) {
        while(++line_ref != end) {
            if(auto trimed = line_ref->trim();
               trimed.empty() || trimed.starts_with('@') || trimed.starts_with('\\')) {
                // Empty line or next command
                if(trimed.empty()) {
                    ++line_ref;
                }
                break;
            }
            process_non_command_line(*line_ref, os);
        }
    };

    if(auto trimed = line_ref->trim();
       !trimed.empty() && (trimed.starts_with('@') || trimed.starts_with('\\'))) {
        // Maybe a doxygen command
        auto command_end = trimed.find_first_of(" \t\v\f\r[");
        llvm::StringRef command, rest_of_line;
        if(command_end == trimed.npos) {
            command = trimed.substr(1);
            rest_of_line = "";
        } else {
            command = trimed.slice(1, command_end);
            rest_of_line = trimed.drop_front(command_end);
        }

        if(command.equals_insensitive("param")) {
            // Got param command
            auto direction = DoxygenInfo::ParamCommandCommentContent::ParamDirection::Unspecified;
            llvm::StringRef param_name;

            if(!rest_of_line.empty()) {
                if(rest_of_line.starts_with('[')) {
                    // Parse direction
                    auto close_bracket = rest_of_line.find(']');
                    if(close_bracket != rest_of_line.npos) {
                        auto param_direction = rest_of_line.slice(1, close_bracket);
                        rest_of_line = rest_of_line.substr(close_bracket + 1);
                        direction =
                            llvm::StringSwitch<
                                DoxygenInfo::ParamCommandCommentContent::ParamDirection>(
                                param_direction)
                                .CaseLower(
                                    "in",
                                    DoxygenInfo::ParamCommandCommentContent::ParamDirection::In)
                                .CaseLower(
                                    "out",
                                    DoxygenInfo::ParamCommandCommentContent::ParamDirection::Out)
                                .CaseLower(
                                    "in,out",
                                    DoxygenInfo::ParamCommandCommentContent::ParamDirection::InOut)
                                .Default(DoxygenInfo::ParamCommandCommentContent::ParamDirection::
                                             Unspecified);
                    } else {
                        // not a closed '[', treat as normal line
                        process_non_command_line(*line_ref, rest);
                        ++line_ref;
                        return;
                    }
                }
                // Parse name
                rest_of_line = rest_of_line.ltrim(" \t\v\f\r");
                if(rest_of_line.empty()) {
                    // Not a legal line, cannot find name
                    ++line_ref;
                    return;
                }
                auto name_end = rest_of_line.find_first_of(" \t\v\f\r");
                if(name_end == llvm::StringRef::npos) {
                    param_name = rest_of_line;
                    rest_of_line = "";
                } else {
                    param_name = rest_of_line.slice(0, name_end);
                    rest_of_line = rest_of_line.drop_front(name_end);
                }

                // Parse rest of the block
                std::string s;
                llvm::raw_string_ostream this_comment_content{s};
                if(!rest_of_line.empty()) {
                    this_comment_content << rest_of_line << '\n';
                }
                consume_command_block(this_comment_content);
                di.add_param_command_comment(param_name, this_comment_content.str(), direction);
                return;
            }

            // line of '@param' only is illegal, escape.
            ++line_ref;
            return;

        } else if(command.equals_insensitive("return")) {
            // Got return command
            std::string s;
            llvm::raw_string_ostream this_comment_content{s};
            if(!rest_of_line.empty()) {
                this_comment_content << rest_of_line << '\n';
            }
            consume_command_block(this_comment_content);
            di.add_return_info(this_comment_content.str());
            return;
        } else {
            // Got normal commands
            std::string s;
            llvm::raw_string_ostream this_comment_content{s};
            if(!rest_of_line.empty()) {
                this_comment_content << rest_of_line << '\n';
            }
            consume_command_block(this_comment_content);
            // Now add to doxygen info and return
            di.add_block_command_comment(command, this_comment_content.str());
            return;
        }
    }
    // Not a command block, but may include commands like '@b', '@e'
    process_non_command_line(*line_ref, rest);
    ++line_ref;
}

std::pair<DoxygenInfo, std::string> strip_doxygen_info(llvm::StringRef raw_comment) {
    DoxygenInfo di;
    std::string s;
    llvm::raw_string_ostream os{s};
    llvm::SmallVector<llvm::StringRef> lines;
    raw_comment.split(lines, "\n");
    // '\n' is not included in each line
    auto line_ref = lines.begin();
    while(line_ref != lines.end()) {
        process_paragragh(line_ref, lines.end(), di, os);
    }
    return {di, os.str()};
}

}  // namespace clice
