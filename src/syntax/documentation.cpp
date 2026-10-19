module;

#include "modules/prelude.h"

module clice;

import :support.markup;
import :syntax.documentation;

namespace clice {

bool Documentation::empty() const {
    return brief.empty() && details.empty() && params.empty() && template_params.empty() &&
           returns.empty() && retvals.empty() && throws.empty() && !deprecated && see.empty();
}

const Documentation::Param* Documentation::param(llvm::StringRef name) const {
    auto it = llvm::find_if(params, [&](const Param& param) { return param.name == name; });
    return it == params.end() ? nullptr : &*it;
}

const Documentation::Param* Documentation::template_param(llvm::StringRef name) const {
    auto it =
        llvm::find_if(template_params, [&](const Param& param) { return param.name == name; });
    return it == template_params.end() ? nullptr : &*it;
}

namespace {

enum class Kind : std::uint8_t {
    /// Paragraphs a blank line or another of them ends.
    Brief,
    Details,
    Param,
    TParam,
    Returns,
    Retval,
    Throws,
    Deprecated,
    See,
    /// A paragraph headed by the command's title: `@note`, `@warning`.
    Note,
    /// A list item: `@li`, `@arg`.
    Item,
    /// `@par [title]`: a paragraph under its title.
    Par,
    /// `@section name title` and its kin: the title alone.
    Heading,

    /// Lines up to the closing command: `@code` and `@verbatim` show as
    /// code, a display formula and a diagram as their source.
    Code,
    /// Lines up to the closing command, shown nowhere: `@htmlonly`,
    /// `@internal`.
    Hidden,

    CopyDoc,
    CopyBrief,
    CopyDetails,

    /// Drops the rest of its line: what a block documents spelled out
    /// (`@fn`), grouping (`@ingroup`), files to include (`@snippet`).
    DropLine,
    /// Drops the command alone: `@{`, `@}`.
    Drop,
    /// The comment documents a file, a page or a group.
    Elsewhere,

    /// Inline: the next word as code (`@c`, `@p`), emphasized (`@a`,
    /// `@e`, `@em`) or bold (`@b`).
    Monospace,
    Emphasis,
    Bold,
    /// Inline `@n`.
    LineBreak,
    /// Inline `@ref name ["text"]`, `@cite name`: the text or the name.
    Ref,
    /// Inline `@link target text @endlink`: the text.
    Link,
    /// Inline `@f$ formula @f$` and `@f( formula @f)`: the formula as code.
    Formula,
    /// Inline `@anchor name`: nothing.
    Anchor,
};

struct Command {
    Kind kind;

    /// The heading of a Note; the closing command of a block, a link or
    /// an inline formula; the language a block's code is shown in.
    llvm::StringRef title = {};
    llvm::StringRef end = {};
    llvm::StringRef language = {};
};

const llvm::StringMap<Command>& commands() {
    const static llvm::StringMap<Command> table = [] {
        llvm::StringMap<Command> table;
        auto add = [&](std::initializer_list<llvm::StringRef> names, Command command) {
            for(auto name: names) {
                table.try_emplace(name, command);
            }
        };
        add({"brief", "short"}, {Kind::Brief});
        add({"details", "detailed"}, {Kind::Details});
        add({"param"}, {Kind::Param});
        add({"tparam"}, {Kind::TParam});
        add({"return", "returns", "result"}, {Kind::Returns});
        add({"retval"}, {Kind::Retval});
        add({"throw", "throws", "exception"}, {Kind::Throws});
        add({"deprecated"}, {Kind::Deprecated});
        add({"see", "sa"}, {Kind::See});

        add({"note"}, {.kind = Kind::Note, .title = "Note"});
        add({"warning"}, {.kind = Kind::Note, .title = "Warning"});
        add({"attention"}, {.kind = Kind::Note, .title = "Attention"});
        add({"important"}, {.kind = Kind::Note, .title = "Important"});
        add({"remark", "remarks"}, {.kind = Kind::Note, .title = "Remark"});
        add({"pre"}, {.kind = Kind::Note, .title = "Precondition"});
        add({"post"}, {.kind = Kind::Note, .title = "Postcondition"});
        add({"invariant"}, {.kind = Kind::Note, .title = "Invariant"});
        add({"since"}, {.kind = Kind::Note, .title = "Since"});
        add({"version"}, {.kind = Kind::Note, .title = "Version"});
        add({"author", "authors"}, {.kind = Kind::Note, .title = "Author"});
        add({"date"}, {.kind = Kind::Note, .title = "Date"});
        add({"copyright"}, {.kind = Kind::Note, .title = "Copyright"});
        add({"todo"}, {.kind = Kind::Note, .title = "Todo"});
        add({"bug"}, {.kind = Kind::Note, .title = "Bug"});
        add({"test"}, {.kind = Kind::Note, .title = "Test"});

        add({"li", "arg", "o", "value"}, {Kind::Item});
        add({"par"}, {Kind::Par});
        add({"section", "subsection", "subsubsection", "paragraph"}, {Kind::Heading});

        add({"code"}, {.kind = Kind::Code, .end = "endcode"});
        add({"verbatim"}, {.kind = Kind::Code, .end = "endverbatim"});
        add({"f["}, {.kind = Kind::Code, .end = "f]", .language = "tex"});
        add({"f{"}, {.kind = Kind::Code, .end = "f}", .language = "tex"});
        add({"dot"}, {.kind = Kind::Code, .end = "enddot", .language = "dot"});
        add({"msc"}, {.kind = Kind::Code, .end = "endmsc"});
        add({"startuml"}, {.kind = Kind::Code, .end = "enduml"});
        add({"htmlonly"}, {.kind = Kind::Hidden, .end = "endhtmlonly"});
        add({"latexonly"}, {.kind = Kind::Hidden, .end = "endlatexonly"});
        add({"rtfonly"}, {.kind = Kind::Hidden, .end = "endrtfonly"});
        add({"manonly"}, {.kind = Kind::Hidden, .end = "endmanonly"});
        add({"xmlonly"}, {.kind = Kind::Hidden, .end = "endxmlonly"});
        add({"docbookonly"}, {.kind = Kind::Hidden, .end = "enddocbookonly"});
        add({"internal"}, {.kind = Kind::Hidden, .end = "endinternal"});

        add({"copydoc"}, {Kind::CopyDoc});
        add({"copybrief"}, {Kind::CopyBrief});
        add({"copydetails"}, {Kind::CopyDetails});

        add({"fn",
             "class",
             "struct",
             "union",
             "enum",
             "var",
             "def",
             "typedef",
             "namespace",
             "property",
             "interface",
             "protocol",
             "category",
             "concept",
             "headerfile",
             "ingroup",
             "name",
             "overload",
             "relates",
             "related",
             "relatesalso",
             "memberof",
             "extends",
             "implements",
             "snippet",
             "snippetdoc",
             "snippetlineno",
             "include",
             "includedoc",
             "includelineno",
             "dontinclude",
             "image",
             "dotfile",
             "mscfile",
             "diafile",
             "cond",
             "endcond",
             "callgraph",
             "callergraph",
             "hidecallgraph",
             "hidecallergraph",
             "showrefs",
             "showrefby",
             "hiderefs",
             "hiderefby",
             "nosubgrouping",
             "private",
             "protected",
             "public",
             "privatesection",
             "protectedsection",
             "publicsection",
             "pure",
             "static",
             "qualifier",
             "showinitializer",
             "hideinitializer",
             "addindex",
             "line",
             "skip",
             "skipline",
             "until",
             "list",
             "endlist",
             "inmodule",
             "target",
             "keyword",
             "reentrant",
             "threadsafe"},
            {Kind::DropLine});
        add({"{", "}"}, {Kind::Drop});
        add({"file", "mainpage", "page", "defgroup", "addtogroup", "weakgroup", "dir", "example"},
            {Kind::Elsewhere});

        add({"c", "p"}, {Kind::Monospace});
        add({"a", "e", "em"}, {Kind::Emphasis});
        add({"b"}, {Kind::Bold});
        add({"n"}, {Kind::LineBreak});
        add({"ref", "cite"}, {Kind::Ref});
        add({"link"}, {.kind = Kind::Link, .end = "endlink"});
        add({"f$"}, {.kind = Kind::Formula, .end = "f$"});
        add({"f("}, {.kind = Kind::Formula, .end = "f)"});
        add({"anchor"}, {Kind::Anchor});
        return table;
    }();
    return table;
}

bool inline_kind(Kind kind) {
    return kind >= Kind::Monospace;
}

bool identifier_char(char c) {
    return llvm::isAlnum(c) || c == '_';
}

/// The name of a documentation command whose marker is right before
/// `text`: a word, or one of the formula delimiters and the group braces.
llvm::StringRef doc_command_name(llvm::StringRef text) {
    if(text.size() >= 2 && text[0] == 'f' && llvm::StringRef("$[]{}()").contains(text[1])) {
        return text.take_front(2);
    }
    if(text.starts_with("{") || text.starts_with("}")) {
        return text.take_front(1);
    }
    if(text.empty() || !llvm::isAlpha(text[0])) {
        return {};
    }
    return text.take_while(identifier_char);
}

/// Whether a documentation command may start at `index`: Doxygen reads `@`
/// and `\` as one only at the start of a word, as `user@example.com` and
/// `C:\path` show.
bool at_doc_word_start(llvm::StringRef line, std::size_t index) {
    return index == 0 || llvm::isSpace(line[index - 1]) ||
           llvm::StringRef("([{\"'*_").contains(line[index - 1]);
}

/// Where `closer`, the command ending a raw block, is written in `text`
/// with either marker (`@endcode`, `\endcode`) and no longer name; npos if
/// nowhere.
std::size_t find_doc_closer(llvm::StringRef text, llvm::StringRef closer) {
    auto end = llvm::StringRef::npos;
    for(llvm::StringRef marker: {"\\", "@"}) {
        auto at = text.find((marker + closer).str());
        if(at == llvm::StringRef::npos) {
            continue;
        }
        auto after = text.substr(at + 1 + closer.size());
        if(after.empty() || !identifier_char(after.front())) {
            end = std::min(end, at);
        }
    }
    return end;
}

/// The word an inline command takes from `text`: up to the next blank,
/// without the punctuation that closes a sentence or a parenthesis it did
/// not open.
llvm::StringRef next_word(llvm::StringRef text) {
    auto word = text.ltrim(" \t").take_until([](char c) { return c == ' ' || c == '\t'; });
    word = word.rtrim(".,;:!?");
    while(word.ends_with(")") && word.count('(') < word.count(')')) {
        word = word.drop_back();
    }
    return word;
}

/// The text after `word`, which starts `text` past its leading blanks.
llvm::StringRef after_word(llvm::StringRef text, llvm::StringRef word) {
    text = text.ltrim(" \t");
    return text.substr(word.size());
}

/// The parameters of a parameter list, split at the commas outside
/// parentheses, brackets, braces, angle brackets and literals.
llvm::SmallVector<llvm::StringRef> split_parameters(llvm::StringRef list) {
    llvm::SmallVector<llvm::StringRef> parameters;
    if(list.trim().empty() || list.trim() == "void") {
        return parameters;
    }
    int depth = 0;
    std::size_t begin = 0;
    for(std::size_t index = 0; index < list.size(); index += 1) {
        auto c = list[index];
        bool literal = c == '"';
        if(c == '\'') {
            // A `'` inside a number separates its digits.
            auto before = list.take_front(index);
            auto word = before.substr(before.rfind_if_not(identifier_char) + 1);
            literal = word.empty() || word == "L" || word == "u" || word == "U" || word == "u8";
        }
        if(literal) {
            for(index += 1; index < list.size() && list[index] != c; index += 1) {
                if(list[index] == '\\') {
                    index += 1;
                }
            }
        } else if(llvm::StringRef("([{<").contains(c)) {
            depth += 1;
        } else if(llvm::StringRef(")]}>").contains(c)) {
            depth -= 1;
        } else if(c == ',' && depth == 0) {
            parameters.push_back(list.slice(begin, index));
            begin = index + 1;
        }
    }
    parameters.push_back(list.substr(begin));
    return parameters;
}

/// A parameter without its default argument, a blank kept only between
/// two words: `const T &x = {}` is `const T&x`.
std::string normalized_parameter(llvm::StringRef parameter) {
    std::string result;
    bool blank = false;
    int depth = 0;
    for(auto c: parameter) {
        if(llvm::StringRef("([{<").contains(c)) {
            depth += 1;
        } else if(llvm::StringRef(")]}>").contains(c)) {
            depth -= 1;
        } else if(c == '=' && depth == 0) {
            break;
        }
        if(llvm::isSpace(c)) {
            blank = true;
            continue;
        }
        if(blank && !result.empty() && identifier_char(result.back()) && identifier_char(c)) {
            result += ' ';
        }
        blank = false;
        result += c;
    }
    return result;
}

/// Whether `longer` is `shorter` with a parameter name after it.
bool adds_name(llvm::StringRef longer, llvm::StringRef shorter) {
    if(shorter.empty() || !longer.starts_with(shorter) || longer.size() == shorter.size()) {
        return false;
    }
    auto name = longer.substr(shorter.size());
    if(identifier_char(shorter.back())) {
        if(!name.consume_front(" ")) {
            return false;
        }
    }
    // `long long` is no `long` named `long`.
    const static llvm::StringSet<> type_words = {"char",
                                                 "short",
                                                 "int",
                                                 "long",
                                                 "signed",
                                                 "unsigned",
                                                 "float",
                                                 "double",
                                                 "const",
                                                 "volatile"};
    return !name.empty() && llvm::all_of(name, identifier_char) && !type_words.contains(name);
}

/// Whether two normalized parameters are one, a name only one of them
/// gives aside, also before an array's bounds: `int values[4]` is `int[4]`.
bool same_parameter(llvm::StringRef have, llvm::StringRef want) {
    auto bounds = [](llvm::StringRef parameter) {
        int depth = 0;
        for(std::size_t index = 0; index < parameter.size(); index += 1) {
            auto c = parameter[index];
            if(c == '[' && depth == 0) {
                return parameter.substr(index);
            }
            depth += llvm::StringRef("(<{").contains(c)   ? 1
                     : llvm::StringRef(")>}").contains(c) ? -1
                                                          : 0;
        }
        return llvm::StringRef();
    };
    if(bounds(have) != bounds(want)) {
        return false;
    }
    have = have.drop_back(bounds(have).size());
    want = want.drop_back(bounds(want).size());
    return have == want || adds_name(have, want) || adds_name(want, have);
}

/// A line opening a Markdown block a paragraph cannot hold: a list item,
/// a table line, a heading, a quote, HTML, a code fence or indented code.
bool opens_block(llvm::StringRef line) {
    auto indent = line.size() - line.ltrim(" ").size();
    if(indent >= 4) {
        return true;
    }
    line = line.ltrim(" ");
    if(line.starts_with("- ") || line.starts_with("* ") || line.starts_with("+ ") ||
       line.starts_with("|") || line.starts_with("#") || line.starts_with(">") ||
       line.starts_with("<") || line.starts_with("```") || line.starts_with("~~~")) {
        return true;
    }
    auto digits = line.take_while(llvm::isDigit);
    return !digits.empty() && (line.substr(digits.size()).starts_with(". ") ||
                               line.substr(digits.size()).starts_with(") "));
}

/// GTK-Doc annotations ahead of a parameter's or a return value's text,
/// `(transfer none) (nullable):`, taken off.
llvm::StringRef drop_annotations(llvm::StringRef text) {
    auto rest = text.ltrim();
    bool any = false;
    while(rest.starts_with("(")) {
        auto close = rest.find(')');
        if(close == llvm::StringRef::npos) {
            return text;
        }
        rest = rest.substr(close + 1).ltrim();
        any = true;
    }
    if(any && rest.consume_front(":")) {
        return rest.ltrim();
    }
    return text.ltrim();
}

/// GTK-Doc's name line, `gtk_widget_show:`, `GtkWidget::show:` or
/// `GtkWidget:visible:`, annotations allowed after it. `strong` is set when
/// the name could not be an English word ending a sentence like `Usage:`.
bool gtk_name_line(llvm::StringRef line, bool& strong) {
    auto name = line.take_while(identifier_char);
    if(name.empty() || (!llvm::isAlpha(name[0]) && name[0] != '_')) {
        return false;
    }
    strong = name.contains('_');
    auto rest = line.substr(name.size());
    while(true) {
        auto separator = rest.starts_with("::") ? 2 : rest.starts_with(":") ? 1 : 0;
        auto part = rest.substr(separator).take_while(
            [](char c) { return identifier_char(c) || c == '-'; });
        if(separator == 0 || part.empty()) {
            break;
        }
        strong = true;
        rest = rest.substr(separator + part.size());
    }
    if(!rest.consume_front(":")) {
        return false;
    }
    rest = rest.trim();
    while(rest.starts_with("(") && rest.contains(')')) {
        rest = rest.substr(rest.find(')') + 1).ltrim();
    }
    return rest.empty();
}

/// kernel-doc's name line, `foo() - brief` or `struct foo - brief`: the
/// brief when `line` is one. `loose` takes a function written without its
/// parentheses, `foo - brief`, as many kernel comments write it: for a
/// comment the rest of which reads as kernel-doc.
std::optional<llvm::StringRef> kernel_name_line(llvm::StringRef line, bool loose) {
    auto rest = line;
    bool named = false;
    for(llvm::StringRef keyword: {"struct ", "union ", "enum ", "typedef "}) {
        if(rest.consume_front(keyword)) {
            named = true;
            break;
        }
    }
    auto name = rest.ltrim().take_while(identifier_char);
    if(name.empty()) {
        return std::nullopt;
    }
    rest = rest.ltrim().substr(name.size());
    if(!named && !rest.consume_front("()") && !loose) {
        return std::nullopt;
    }
    if(!rest.ltrim().starts_with("- ") && rest.trim() != "-") {
        return std::nullopt;
    }
    return rest.ltrim().drop_front(1).trim();
}

/// A GTK-Doc section line, `Returns: text`: its kind and title, and the
/// text after it.
std::optional<std::pair<Command, llvm::StringRef>> gtk_section(llvm::StringRef line) {
    constexpr static std::pair<llvm::StringRef, Command> sections[] = {
        {"Returns:",      {Kind::Returns}                           },
        {"Return value:", {Kind::Returns}                           },
        {"Return:",       {Kind::Returns}                           },
        {"Deprecated:",   {Kind::Deprecated}                        },
        {"Since:",        {.kind = Kind::Note, .title = "Since"}    },
        {"Stability:",    {.kind = Kind::Note, .title = "Stability"}},
        {"Context:",      {.kind = Kind::Note, .title = "Context"}  },
    };
    for(auto& [label, command]: sections) {
        if(line.starts_with(label)) {
            return std::pair(command, drop_annotations(line.substr(label.size())));
        }
    }
    return std::nullopt;
}

/// The comment's lines read one by one into a Documentation.
class Parser {
public:
    explicit Parser(CopiedDocLookup lookup) : lookup(lookup) {}

    Documentation run(llvm::StringRef text) {
        llvm::SmallVector<llvm::StringRef> lines;
        text.split(lines, '\n');

        // GTK-Doc and kernel-doc name what a comment documents on its
        // first line.
        auto first =
            llvm::find_if(lines, [](llvm::StringRef line) { return !line.trim().empty(); });
        if(first != lines.end()) {
            auto line = first->trim();
            bool strong = false;
            if(line.starts_with("SECTION:") || line.starts_with("DOC:")) {
                return {};
            }
            auto marks_gtk = [](llvm::StringRef line) {
                line = line.trim();
                return gtk_section(line).has_value() ||
                       (line.starts_with("@") &&
                        line.drop_front().drop_while(identifier_char).starts_with(":"));
            };
            bool marked = llvm::any_of(lines, marks_gtk);
            if(auto brief = kernel_name_line(line, marked)) {
                gtk = true;
                doc.brief = brief->str();
                *first = {};
            } else if(gtk_name_line(line, strong) && (strong || marked)) {
                gtk = true;
                *first = {};
            }
        }

        for(auto line: lines) {
            feed_line(line);
        }
        // A block the comment leaves open ends with it.
        if(!fence.empty()) {
            push_raw(fence);
        } else if(!block_end.empty() && !hidden) {
            push_raw("```");
        }
        close();
        if(elsewhere) {
            return {};
        }

        finish_brief();
        while(!details.empty() && details.back().empty()) {
            details.pop_back();
        }
        doc.details = llvm::join(details, "\n");
        return std::move(doc);
    }

private:
    void feed_line(llvm::StringRef line) {
        line = line.rtrim();
        if(!fence.empty()) {
            push_raw(line.str());
            auto trimmed = line.ltrim();
            if(trimmed.starts_with(fence) && trimmed.ltrim(fence.front()).trim().empty()) {
                fence.clear();
            }
            return;
        }
        if(!block_end.empty()) {
            if(auto rest = block_line(line); !rest.empty()) {
                prose(rest);
            }
            return;
        }

        auto trimmed = line.trim();
        if(trimmed.empty()) {
            close();
            push_blank();
            indented_code = false;
            return;
        }

        if(section == Kind::Details) {
            auto indent = line.size() - line.ltrim(" ").size();
            if(indent >= 4 && (indented_code || details.empty() || details.back().empty())) {
                indented_code = true;
                push_raw(line.str());
                return;
            }
            indented_code = false;
        }

        if(trimmed.starts_with("```") || trimmed.starts_with("~~~")) {
            close();
            push_blank();
            fence = trimmed.take_while([&](char c) { return c == trimmed.front(); }).str();
            push_raw(line.str());
            return;
        }

        // GTK-Doc's `@widget: text` documents a parameter; with a
        // Doxygen command's name, the colon is the writer's own.
        if(trimmed.starts_with("@")) {
            auto name = trimmed.drop_front().take_while(identifier_char);
            if(trimmed.drop_front().starts_with("...")) {
                name = trimmed.substr(1, 3);
            }
            auto rest = trimmed.substr(1 + name.size());
            // `@TODO:` marks a line, as `TODO:` does, outside GTK-Doc.
            bool marker = !gtk && name.size() > 1 &&
                          llvm::all_of(name, [](char c) { return llvm::isUpper(c) || c == '_'; });
            if(!name.empty() && rest.starts_with(":") && !marker) {
                if(gtk || !commands().contains(name)) {
                    close();
                    section = Kind::Param;
                    names.push_back(name.str());
                    prose(drop_annotations(rest.drop_front()));
                    return;
                }
                prose(("@" + name + rest.drop_front()).str());
                return;
            }
        }

        if(gtk) {
            if(auto found = gtk_section(trimmed)) {
                close();
                start_section(found->first);
                prose(found->second);
                return;
            }
        }

        prose(section == Kind::Details ? line : trimmed);
    }

    /// One line of a block a command opened: code up to the closing
    /// command, and what follows that command on the line.
    llvm::StringRef block_line(llvm::StringRef line) {
        auto end = find_doc_closer(line, block_end);
        auto code = line.substr(0, end);
        if(!hidden && !(end != llvm::StringRef::npos && code.trim().empty())) {
            push_raw(code.rtrim().str());
        }
        if(end == llvm::StringRef::npos) {
            return {};
        }
        if(!hidden) {
            push_raw("```");
        }
        auto rest = line.substr(end + 1 + block_end.size());
        block_end = {};
        hidden = false;
        return rest;
    }

    /// Read a line of text: inline commands become Markdown, block commands
    /// switch to the section they open.
    void prose(llvm::StringRef line) {
        std::string out;
        std::size_t index = 0;
        while(index < line.size()) {
            char c = line[index];

            if(c == '`') {
                auto run = line.substr(index).take_while([](char c) { return c == '`'; });
                auto close = line.find(run, index + run.size());
                auto end = close == llvm::StringRef::npos ? index + run.size() : close + run.size();
                out += line.slice(index, end);
                index = end;
                continue;
            }

            if(c == '\\' && index + 1 < line.size()) {
                if(line.substr(index + 1).starts_with("::")) {
                    out += "::";
                    index += 3;
                    continue;
                }
                if(llvm::StringRef(R"(\@&$#<>%".|)").contains(line[index + 1])) {
                    out += line.substr(index, 2);
                    index += 2;
                    continue;
                }
            }

            if((c == '\\' || c == '@') && at_doc_word_start(line, index)) {
                auto name = doc_command_name(line.substr(index + 1));
                auto found = commands().find(name);
                if(!name.empty() && found != commands().end() && (!gtk || c == '\\')) {
                    auto rest = line.substr(index + 1 + name.size());
                    auto& command = found->second;
                    if(inline_kind(command.kind)) {
                        rest = inline_command(command, rest, out);
                    } else {
                        emit(out);
                        out.clear();
                        rest = block_command(command, rest);
                    }
                    line = rest;
                    index = 0;
                    continue;
                }
            }

            if(gtk && (c == '#' || c == '%' || c == '@') && at_doc_word_start(line, index) &&
               index + 1 < line.size() &&
               (llvm::isAlpha(line[index + 1]) || line[index + 1] == '_')) {
                auto word = line.substr(index + 1).take_while(
                    [](char c) { return identifier_char(c) || c == ':' || c == '-'; });
                word = word.rtrim(":-");
                out += markup::code_span(word);
                index += 1 + word.size();
                continue;
            }

            // Doxygen's `%` keeps a word from becoming a link and is not
            // shown; short lowercase words after one are printf formats.
            if(c == '%' && at_doc_word_start(line, index)) {
                auto word = line.substr(index + 1).take_while(identifier_char);
                if(word.size() >= 4 ||
                   llvm::any_of(word, [](char c) { return llvm::isUpper(c) || c == '_'; })) {
                    out += word;
                    index += 1 + word.size();
                    continue;
                }
            }

            out += c;
            index += 1;
        }
        emit(out);
    }

    /// Write an inline command to `out`; the text after its arguments.
    llvm::StringRef inline_command(const Command& command, llvm::StringRef rest, std::string& out) {
        auto closing = [&](llvm::StringRef text) -> std::pair<llvm::StringRef, llvm::StringRef> {
            for(auto at = text.find(command.end); at != llvm::StringRef::npos;
                at = text.find(command.end, at + 1)) {
                if(at > 0 && (text[at - 1] == '\\' || text[at - 1] == '@')) {
                    return {text.substr(0, at - 1), text.substr(at + command.end.size())};
                }
            }
            return {text, {}};
        };

        switch(command.kind) {
            case Kind::Monospace:
            case Kind::Emphasis:
            case Kind::Bold: {
                auto word = next_word(rest);
                if(word.empty()) {
                    return rest;
                }
                if(command.kind == Kind::Monospace) {
                    out += markup::code_span(word);
                } else {
                    auto marker = command.kind == Kind::Bold ? "**" : "*";
                    out += marker + word.str() + marker;
                }
                return after_word(rest, word);
            }
            case Kind::LineBreak: {
                out += "  \n";
                return rest.ltrim(" \t");
            }
            case Kind::Ref: {
                auto word = next_word(rest);
                rest = after_word(rest, word);
                if(auto quoted = rest.ltrim(" \t");
                   quoted.starts_with("\"") && quoted.drop_front().contains('"')) {
                    auto text = quoted.drop_front().take_until([](char c) { return c == '"'; });
                    out += text;
                    return quoted.substr(text.size() + 2);
                }
                out += word;
                return rest;
            }
            case Kind::Link: {
                auto target = next_word(rest);
                auto [text, after] = closing(after_word(rest, target));
                out += text.trim().empty() ? target : text.trim();
                return after;
            }
            case Kind::Formula: {
                auto [formula, after] = closing(rest);
                out += markup::code_span(formula.trim());
                return after;
            }
            case Kind::Anchor: {
                return after_word(rest, next_word(rest));
            }
            default: std::unreachable();
        }
    }

    /// Open the section or the block of a command; the text left on its
    /// line, read as that section's.
    llvm::StringRef block_command(const Command& command, llvm::StringRef rest) {
        switch(command.kind) {
            case Kind::Code:
            case Kind::Hidden: {
                close();
                hidden = command.kind == Kind::Hidden;
                block_end = command.end;
                if(!hidden) {
                    push_blank();
                    auto language = command.language;
                    if(rest.starts_with("{")) {
                        language = rest.drop_front().take_until([](char c) { return c == '}'; });
                        rest = rest.substr(language.size() + 2);
                        language.consume_front(".");
                    }
                    push_raw(("```" + language).str());
                }
                return rest.trim().empty() ? llvm::StringRef() : block_line(rest);
            }
            case Kind::CopyDoc:
            case Kind::CopyBrief:
            case Kind::CopyDetails: {
                rest = rest.ltrim(" \t");
                auto name =
                    rest.take_until([](char c) { return c == ' ' || c == '\t' || c == '('; });
                if(rest.substr(name.size()).starts_with("(")) {
                    int depth = 0;
                    auto close = std::find_if(rest.begin() + name.size(), rest.end(), [&](char c) {
                        depth += c == '(' ? 1 : c == ')' ? -1 : 0;
                        return depth == 0;
                    });
                    name = rest.take_front(close == rest.end() ? rest.size()
                                                               : close - rest.begin() + 1);
                }
                copy(command.kind, name);
                return rest.substr(name.size());
            }
            case Kind::DropLine: return {};
            case Kind::Drop: return rest;
            case Kind::Elsewhere: {
                elsewhere = true;
                return {};
            }
            default: break;
        }

        close();
        start_section(command);
        switch(command.kind) {
            case Kind::Param: {
                rest = rest.ltrim(" \t");
                if(rest.starts_with("[")) {
                    auto direction = rest.drop_front().take_until([](char c) { return c == ']'; });
                    rest = rest.substr(direction.size() + 2);
                    this->direction = direction.trim().str();
                    llvm::erase(this->direction, ' ');
                }
                auto word =
                    rest.ltrim(" \t").take_until([](char c) { return c == ' ' || c == '\t'; });
                for(auto name: llvm::split(word.rtrim(":"), ',')) {
                    if(!name.trim().empty()) {
                        names.push_back(name.trim().str());
                    }
                }
                return description(after_word(rest, word));
            }
            case Kind::TParam:
            case Kind::Retval:
            case Kind::Throws: {
                auto word =
                    rest.ltrim(" \t").take_until([](char c) { return c == ' ' || c == '\t'; });
                if(!word.rtrim(":").empty()) {
                    names.push_back(word.rtrim(":").str());
                }
                return description(after_word(rest, word));
            }
            case Kind::Par: {
                title = rest.trim().str();
                return {};
            }
            case Kind::Heading: {
                auto word = next_word(rest);
                title = after_word(rest, word).trim().str();
                if(title.empty()) {
                    title = word.str();
                }
                return {};
            }
            default: return rest;
        }
    }

    /// The text of an entry after its name, without the dash or colon
    /// some write between.
    static llvm::StringRef description(llvm::StringRef text) {
        text = text.ltrim(" \t");
        if(text.starts_with("- ") || text.starts_with(": ") || text == "-" || text == ":") {
            text = text.drop_front().ltrim(" \t");
        }
        return text;
    }

    /// `@copybrief` copies the brief, `@copydetails` the rest, `@copydoc`
    /// both; what the rest documents by name joins what this comment
    /// documents, which keeps its own.
    void copy(Kind kind, llvm::StringRef name) {
        auto copied = lookup(name);
        if(!copied) {
            push_line("See " + markup::code_span(name) + ".", true);
            return;
        }
        close();
        if(kind != Kind::CopyDetails && !copied->brief.empty()) {
            push_blank();
            push_line(copied->brief, true);
            push_blank();
        }
        if(kind == Kind::CopyBrief) {
            return;
        }
        if(!copied->details.empty()) {
            push_blank();
            for(auto line: llvm::split(copied->details, '\n')) {
                push_raw(line.str());
            }
            push_blank();
        }

        for(auto& param: copied->params) {
            if(!doc.param(param.name)) {
                doc.params.push_back(param);
            }
        }
        for(auto& param: copied->template_params) {
            if(!doc.template_param(param.name)) {
                doc.template_params.push_back(param);
            }
        }
        if(doc.returns.empty()) {
            doc.returns = copied->returns;
        }
        llvm::append_range(doc.retvals, copied->retvals);
        llvm::append_range(doc.throws, copied->throws);
        llvm::append_range(doc.see, copied->see);
        if(!doc.deprecated) {
            doc.deprecated = copied->deprecated;
        }
    }

    void start_section(const Command& command) {
        section = command.kind;
        title = command.title.str();
        bool in_list = !details.empty() && llvm::StringRef(details.back()).starts_with("- ");
        if(command.kind == Kind::Details || (command.kind == Kind::Item && !in_list)) {
            push_blank();
        }
    }

    /// Text read in the current section.
    void emit(llvm::StringRef text) {
        if(section == Kind::Details) {
            if(!text.trim().empty()) {
                push_line(text.rtrim().str(), true);
            }
            return;
        }
        auto trimmed = text.trim();
        if(trimmed.empty()) {
            return;
        }
        if(!this->text.empty()) {
            this->text += ' ';
        }
        this->text += trimmed;
    }

    /// End the section being written: its text goes where it belongs.
    void close() {
        auto text = std::exchange(this->text, {});
        auto names = std::exchange(this->names, {});
        auto direction = std::exchange(this->direction, {});
        auto title = std::exchange(this->title, {});
        auto kind = std::exchange(section, Kind::Details);

        // An entry whose name went to the next line.
        if(names.empty() && !text.empty() &&
           llvm::is_contained({Kind::Param, Kind::TParam, Kind::Retval, Kind::Throws}, kind)) {
            auto name = llvm::StringRef(text).take_until(llvm::isSpace);
            names.push_back(name.str());
            text = description(llvm::StringRef(text).substr(name.size())).str();
        }

        auto append = [](std::string& to, llvm::StringRef text) {
            if(!to.empty() && !text.empty()) {
                to += ' ';
            }
            to += text;
        };
        auto add_params = [&](std::vector<Documentation::Param>& params) {
            for(auto& name: names) {
                auto it = llvm::find_if(params, [&](auto& param) { return param.name == name; });
                if(it == params.end()) {
                    params.push_back({.name = name, .direction = direction, .text = text});
                } else {
                    append(it->text, text);
                }
            }
        };

        switch(kind) {
            case Kind::Details: break;
            case Kind::Brief: append(doc.brief, text); break;
            case Kind::Param: add_params(doc.params); break;
            case Kind::TParam: add_params(doc.template_params); break;
            case Kind::Returns: append(doc.returns, text); break;
            case Kind::Retval:
            case Kind::Throws: {
                auto& entries = kind == Kind::Retval ? doc.retvals : doc.throws;
                for(auto& name: names) {
                    entries.push_back({.name = name, .text = text});
                }
                break;
            }
            case Kind::Deprecated: append(doc.deprecated.emplace(), text); break;
            case Kind::See: {
                if(!text.empty()) {
                    doc.see.push_back(std::move(text));
                }
                break;
            }
            case Kind::Note: {
                push_paragraph(text.empty() ? "**" + title + "**" : "**" + title + ":** " + text);
                break;
            }
            case Kind::Item: {
                push_line("- " + text, false);
                break;
            }
            case Kind::Par:
            case Kind::Heading: {
                if(!title.empty()) {
                    push_paragraph("**" + title + "**");
                }
                if(!text.empty()) {
                    push_paragraph(text);
                }
                break;
            }
            default: std::unreachable();
        }
    }

    void push_line(std::string line, bool free) {
        if(!has_details && !line.empty()) {
            has_details = true;
            starts_free = free;
        }
        details.push_back(std::move(line));
    }

    void push_raw(std::string line) {
        push_line(std::move(line), false);
    }

    void push_blank() {
        if(!details.empty() && !details.back().empty()) {
            details.push_back({});
        }
    }

    void push_paragraph(std::string text) {
        push_blank();
        push_line(std::move(text), false);
        push_blank();
    }

    /// Without `@brief`, the first paragraph is the brief when a blank
    /// line or a command ends it — not a list, a table or code.
    void finish_brief() {
        auto begin = llvm::find_if(details, [](auto& line) { return !line.empty(); });
        if(!doc.brief.empty() || !starts_free || begin == details.end()) {
            return;
        }
        auto end = std::find_if(begin, details.end(), [](auto& line) { return line.empty(); });
        if(std::any_of(begin, end, [](auto& line) { return opens_block(line); })) {
            return;
        }
        for(auto it = begin; it != end; ++it) {
            if(!doc.brief.empty()) {
                doc.brief += ' ';
            }
            doc.brief += llvm::StringRef(*it).trim();
        }
        details.erase(details.begin(), end);
        while(!details.empty() && details.front().empty()) {
            details.erase(details.begin());
        }
    }

    CopiedDocLookup lookup;
    Documentation doc;

    /// The comment follows GTK-Doc's or kernel-doc's conventions.
    bool gtk = false;

    /// The comment documents a file, a page or a group.
    bool elsewhere = false;

    /// The section being written, with what its command gave.
    Kind section = Kind::Details;
    std::string title;
    std::string direction;
    llvm::SmallVector<std::string, 2> names;
    std::string text;

    /// The lines of the details; whether the first came from free text.
    std::vector<std::string> details;
    bool has_details = false;
    bool starts_free = false;

    /// The fence of the Markdown code block being read; the closing
    /// command of the block a command opened, and whether it is shown.
    std::string fence;
    llvm::StringRef block_end;
    bool hidden = false;
    bool indented_code = false;
};

}  // namespace

Documentation parse_documentation(llvm::StringRef text, CopiedDocLookup lookup) {
    return Parser(lookup).run(text);
}

auto DocCommandScanner::scan(llvm::StringRef text) -> llvm::SmallVector<Part> {
    llvm::SmallVector<Part> parts;
    // The comment's text starts a word right past its marker, `///` or
    // a trailing one, `///<`.
    std::size_t payload = text.size() > 3 && text[3] == '<' ? 4 : 3;
    for(std::size_t i = payload; i < text.size(); i += 1) {
        if(!raw_end.empty()) {
            auto close = find_doc_closer(text.substr(i), raw_end);
            if(close == llvm::StringRef::npos) {
                break;
            }
            i += close;
            parts.push_back(
                {.kind = Part::Kind::Command, .begin = i, .end = i + 1 + raw_end.size()});
            i = parts.back().end - 1;
            raw_end = {};
            continue;
        }
        if((text[i] != '@' && text[i] != '\\') || (i > payload && !at_doc_word_start(text, i))) {
            continue;
        }
        auto name = doc_command_name(text.substr(i + 1));
        auto command = commands().find(name);
        if(command == commands().end()) {
            continue;
        }
        auto command_end = i + 1 + name.size();
        parts.push_back({.kind = Part::Kind::Command, .begin = i, .end = command_end});
        i = command_end - 1;
        // No command is read in a block's code, hidden text or a formula.
        auto kind = command->second.kind;
        if(kind == Kind::Code || kind == Kind::Hidden || kind == Kind::Formula) {
            raw_end = command->second.end;
            continue;
        }
        if(kind != Kind::Param && kind != Kind::TParam) {
            continue;
        }
        // `@param name`, `@param[in] name`, `@param [in] first,second`.
        auto blanks_from = [&](std::size_t at) {
            while(at < text.size() && (text[at] == ' ' || text[at] == '\t')) {
                at += 1;
            }
            return at;
        };
        auto word = blanks_from(command_end);
        if(word < text.size() && text[word] == '[') {
            auto close = text.find(']', word);
            word = blanks_from(close == llvm::StringRef::npos ? text.size() : close + 1);
        }
        while(true) {
            auto word_end = word;
            while(word_end < text.size() && identifier_char(text[word_end])) {
                word_end += 1;
            }
            if(word_end == word) {
                break;
            }
            parts.push_back({
                .kind = kind == Kind::Param ? Part::Kind::Parameter : Part::Kind::TemplateParameter,
                .begin = word,
                .end = word_end,
            });
            i = word_end - 1;
            if(kind != Kind::Param || word_end == text.size() || text[word_end] != ',') {
                break;
            }
            word = word_end + 1;
        }
    }
    return parts;
}

std::optional<CopiedName> parse_copied_name(llvm::StringRef name) {
    CopiedName copied;
    if(auto open = name.find('('); open != llvm::StringRef::npos) {
        auto parameters = name.substr(open + 1);
        parameters.consume_back(")");
        copied.parameters = parameters;
        name = name.take_front(open);
    }
    name = name.trim();
    copied.absolute = name.consume_front("::");
    // Doxygen names a member `Class::member` or `Class#member`.
    for(auto part: llvm::split(name, "::")) {
        for(auto piece: llvm::split(part, '#')) {
            if(piece.trim().empty()) {
                return std::nullopt;
            }
            copied.parts.push_back(piece.trim());
        }
    }
    return copied;
}

bool declares_parameters(llvm::StringRef content, std::uint32_t name_end, llvm::StringRef spelled) {
    auto rest = content.substr(name_end).ltrim();
    if(rest.starts_with("<")) {
        int depth = 0;
        auto close = llvm::find_if(rest, [&](char c) {
            depth += c == '<' ? 1 : c == '>' ? -1 : 0;
            return depth == 0;
        });
        rest = rest.substr(close - rest.begin() + 1).ltrim();
    }
    if(!rest.consume_front("(")) {
        return false;
    }
    int depth = 0;
    auto close = llvm::find_if(rest, [&](char c) {
        depth += c == '(' ? 1 : c == ')' ? -1 : 0;
        return depth < 0;
    });
    if(close == rest.end()) {
        return false;
    }

    auto declared = split_parameters(rest.take_front(close - rest.begin()));
    auto wanted = split_parameters(spelled);
    if(declared.size() != wanted.size()) {
        return false;
    }
    for(auto [declared_parameter, wanted_parameter]: llvm::zip_equal(declared, wanted)) {
        if(!same_parameter(normalized_parameter(declared_parameter),
                           normalized_parameter(wanted_parameter))) {
            return false;
        }
    }
    return true;
}

}  // namespace clice
