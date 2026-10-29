module clice;

import :support.text;
import :syntax.doc_comment;
import :syntax.lexical_scan;

namespace clice {

namespace {

using Comment = LexicalInfo::Comment;

llvm::StringRef text_of(llvm::StringRef content, const Comment& comment) {
    return content.slice(comment.range.begin, comment.range.end);
}

/// Whether the comment says it documents what precedes it: `///<`,
/// `//!<`, `/**<` or `/*!<`.
bool marked_trailing(llvm::StringRef text) {
    return is_doc_comment(text) && text.size() > 3 && text[3] == '<';
}

bool follows_code(llvm::StringRef content, const Comment& comment) {
    return !begins_line(content, comment.range.begin);
}

std::uint32_t column_of(llvm::StringRef content, const Comment& comment) {
    return comment.range.begin - line_begin(content, comment.range.begin);
}

/// Whether only blanks and at most one line break separate the comments.
bool adjacent(llvm::StringRef content, const Comment& first, const Comment& second) {
    auto gap = content.slice(first.range.end, second.range.begin);
    return gap.trim().empty() && gap.count('\n') <= 1;
}

bool has_blank_line(llvm::StringRef text) {
    std::size_t newlines = 0;
    for(char c: text) {
        if(c == '\n') {
            newlines += 1;
            if(newlines == 2) {
                return true;
            }
        } else if(!llvm::isSpace(c)) {
            newlines = 0;
        }
    }
    return false;
}

/// Whether code between a comment and a declaration ends what the comment
/// is about: a statement, a block, a directive or a label such as
/// `public:` (`::` is no label). Code ending in a list it leaves open, a
/// template's parameters (`template <`, `template <class T,`) or a
/// function's (`f(`), holds the declaration: the comment is about what owns
/// the list.
bool separates(llvm::StringRef gap) {
    if(gap.find_first_of(";{}#") != llvm::StringRef::npos) {
        return true;
    }
    for(std::size_t index = gap.find(':'); index != llvm::StringRef::npos;
        index = gap.find(':', index + 2)) {
        if(!gap.substr(index).starts_with("::")) {
            return true;
        }
    }
    auto code = gap.rtrim();
    if(code.empty() || !llvm::StringRef("<(,").contains(code.back())) {
        return false;
    }
    llvm::SmallVector<char> open;
    // FIXME: brackets inside literals count too: in `template <char C = '>',
    // class T>` the `'>'` closes the list, so T takes the template's comment.
    for(char c: code) {
        if(c == '(' || c == '<') {
            open.push_back(c);
        } else if(c == ')') {
            while(!open.empty() && open.pop_back_val() != '(') {}
        } else if(c == '>' && !open.empty() && open.back() == '<') {
            open.pop_back();
        }
    }
    return !open.empty();
}

/// A line made only of the characters banners are drawn with.
bool is_banner(llvm::StringRef line) {
    return !line.trim().empty() && line.find_first_not_of("=-*_/#+ \t") == llvm::StringRef::npos;
}

}  // namespace

DocumentedDecl documented_macro(llvm::StringRef content, LocalSourceRange definition) {
    if(auto hash = content.substr(0, definition.begin).rfind('#'); hash != llvm::StringRef::npos) {
        definition.begin = static_cast<std::uint32_t>(hash);
    }
    return {.range = definition, .trailing = true};
}

bool is_doc_comment(llvm::StringRef text) {
    if(text.starts_with("//")) {
        return (text.starts_with("///") && !text.starts_with("////")) || text.starts_with("//!");
    }
    return (text.starts_with("/**") && !text.starts_with("/**/") && !text.starts_with("/***")) ||
           text.starts_with("/*!");
}

llvm::ArrayRef<LexicalInfo::Comment>
    documenting_comments(llvm::StringRef content,
                         llvm::ArrayRef<LexicalInfo::Comment> comments,
                         DocumentedDecl decl) {
    auto begin_of = [](const Comment& comment) {
        return comment.range.begin;
    };
    auto& range = decl.range;

    llvm::ArrayRef<Comment> trailing;
    auto after = static_cast<std::size_t>(ranges::lower_bound(comments, range.end, {}, begin_of) -
                                          comments.begin());
    if(decl.trailing && after < comments.size() &&
       content.slice(range.end, comments[after].range.begin).find_first_not_of(" \t;,") ==
           llvm::StringRef::npos) {
        auto column = column_of(content, comments[after]);
        auto end = after + 1;
        while(end < comments.size() && adjacent(content, comments[end - 1], comments[end]) &&
              !follows_code(content, comments[end]) &&
              column_of(content, comments[end]) == column) {
            end += 1;
        }
        trailing = comments.slice(after, end - after);
        if(marked_trailing(text_of(content, comments[after]))) {
            return trailing;
        }
    }

    auto at = static_cast<std::size_t>(ranges::lower_bound(comments, range.begin, {}, begin_of) -
                                       comments.begin());
    if(at == 0) {
        return trailing;
    }
    auto& nearest = comments[at - 1];
    auto text = text_of(content, nearest);
    auto gap = content.slice(nearest.range.end, range.begin);
    bool doc = is_doc_comment(text);
    if(marked_trailing(text) || follows_code(content, nearest) || separates(gap) ||
       (!doc && has_blank_line(gap))) {
        return trailing;
    }

    auto first = at - 1;
    while(first > 0) {
        auto& previous = comments[first - 1];
        auto previous_text = text_of(content, previous);
        if(is_doc_comment(previous_text) != doc || marked_trailing(previous_text) ||
           follows_code(content, previous) || !adjacent(content, previous, comments[first])) {
            break;
        }
        first -= 1;
    }
    return comments.slice(first, at - first);
}

bool DocCommentChoice::offer(llvm::StringRef content, llvm::ArrayRef<LexicalInfo::Comment> run) {
    if(doxygen || run.empty()) {
        return doxygen;
    }
    if(is_doc_comment(text_of(content, run.front()))) {
        text = comment_text(content, run);
        doxygen = true;
    } else if(text.empty()) {
        text = comment_text(content, run);
    }
    return doxygen;
}

std::string comment_text(llvm::StringRef content, llvm::ArrayRef<LexicalInfo::Comment> comments) {
    struct Line {
        llvm::StringRef text;

        /// The line an undecorated block comment opens on: its text starts
        /// after the opener, at no column the lines below it are indented
        /// to. Javadoc's ` * ` lines are measured from their star.
        bool opens_block = false;
    };

    llvm::SmallVector<Line> lines;
    for(auto& comment: comments) {
        auto text = text_of(content, comment);
        auto marker = is_doc_comment(text) ? 3 : 2;
        if(marked_trailing(text)) {
            marker += 1;
        }

        if(comment.kind == Comment::Kind::Line) {
            for(auto line: llvm::split(text.drop_front(marker), '\n')) {
                lines.push_back({.text = line.rtrim()});
            }
            continue;
        }

        text = text.drop_front(marker);
        text.consume_back("*/");
        // A closing `**/` draws more stars than it needs.
        auto stars = text.size() - text.rtrim('*').size();
        if(stars > 0 && (stars == text.size() || llvm::isSpace(text[text.size() - stars - 1]))) {
            text = text.drop_back(stars);
        }
        llvm::SmallVector<llvm::StringRef> block;
        text.split(block, '\n');

        // Javadoc decorates every line after the first with a leading `*`.
        bool decorated = block.size() > 1 && ranges::all_of(views::drop(block, 1), [](auto line) {
                             line = line.trim();
                             return line.empty() || line.starts_with("*");
                         });
        for(auto [index, line]: views::enumerate(block)) {
            if(index > 0 && decorated) {
                line = line.ltrim();
                line.consume_front("*");
            }
            lines.push_back({.text = line.rtrim(), .opens_block = index == 0 && !decorated});
        }
    }

    auto blank_or_banner = [](const Line& line) {
        return line.text.empty() || is_banner(line.text);
    };
    llvm::ArrayRef<Line> kept = lines;
    while(!kept.empty() && blank_or_banner(kept.front())) {
        kept = kept.drop_front();
    }
    while(!kept.empty() && blank_or_banner(kept.back())) {
        kept = kept.drop_back();
    }

    std::size_t indent = std::numeric_limits<std::size_t>::max();
    for(const auto& line: kept) {
        if(!line.text.empty() && !line.opens_block) {
            indent = std::min(indent, line.text.size() - line.text.ltrim(" \t").size());
        }
    }

    std::string result;
    for(const auto& line: kept) {
        if(!result.empty()) {
            result += '\n';
        }
        result += line.opens_block ? line.text.ltrim(" \t")
                                   : line.text.drop_front(std::min(indent, line.text.size()));
    }
    if(!llvm::json::isUTF8(result)) {
        result = llvm::json::fixUTF8(result);
    }
    return result;
}

}  // namespace clice
