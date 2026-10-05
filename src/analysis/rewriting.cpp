#include "analysis/rewriting.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <utility>

#include "syntax/lexer.h"
#include "vfs/file_system.h"
#include "vfs/path.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MemoryBuffer.h"
#include "clang/Basic/IdentifierTable.h"

namespace clice::analysis {

namespace {

constexpr std::uint32_t none = ~0u;

constexpr auto posix = llvm::sys::path::Style::posix;

const clang::LangOptions& cxx() {
    const static auto options = raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx23);
    return options;
}

/// A file's lines and what the lexer finds on each.
struct Text {
    enum class Line : std::uint8_t {
        /// Whitespace, comments, and an include guard's directives.
        Blank,
        Code,
        /// The first line of a preprocessor directive.
        Directive,
        /// A directive's continuation line.
        Continued,
    };

    llvm::SmallVector<llvm::StringRef, 0> lines;
    std::vector<std::uint32_t> starts;
    std::vector<Line> kinds;

    /// Per line: the keyword of the directive it starts, its tokens after
    /// the keyword, and an include's operand as written, `<...>` or `"..."`.
    std::vector<llvm::StringRef> keywords;
    std::vector<llvm::SmallVector<llvm::StringRef, 2>> arguments;
    std::vector<llvm::StringRef> operands;

    /// Per directive: its last line.
    std::vector<std::uint32_t> ends;

    /// Per line: the preprocessor conditions open around it, an include
    /// guard's apart.
    std::vector<std::uint32_t> depths;

    /// The tokens outside directives and the line of each.
    std::vector<Token> tokens;
    std::vector<std::uint32_t> token_lines;

    /// The `#ifndef`, `#define` and `#endif` of an include guard, which a
    /// module unit does without.
    llvm::SmallVector<std::uint32_t, 3> guard;

    llvm::StringRef content;

    /// `content` ends at a NUL terminator, as the lexer requires.
    explicit Text(llvm::StringRef content) : content(content) {
        content.split(lines, '\n');
        // The newline ending the last line starts none.
        if(content.ends_with("\n")) {
            lines.pop_back();
        }
        std::uint32_t offset = 0;
        for(auto line: lines) {
            starts.push_back(offset);
            offset += static_cast<std::uint32_t>(line.size()) + 1;
        }
        auto count = lines.size();
        kinds.assign(count, Line::Blank);
        keywords.resize(count);
        arguments.resize(count);
        operands.resize(count);
        ends.resize(count);

        Lexer lexer(content, {.lang_opts = &cxx()});
        for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
            auto line = line_of(token.range.begin);
            if(!token.is_directive_hash()) {
                if(kinds[line] == Line::Blank) {
                    kinds[line] = Line::Code;
                }
                tokens.push_back(token);
                token_lines.push_back(line);
                continue;
            }
            kinds[line] = Line::Directive;
            auto last = line;
            for(auto part = lexer.advance(); !part.is_eof(); part = lexer.advance()) {
                last = line_of(part.range.begin);
                if(part.is_eod()) {
                    break;
                }
                if(part.is_pp_keyword) {
                    keywords[line] = part.text(content);
                } else if(part.is_header_name() && operands[line].empty()) {
                    operands[line] = part.text(content);
                } else {
                    arguments[line].push_back(part.text(content));
                }
            }
            ends[line] = last;
            for(auto continued = line + 1; continued <= last; continued += 1) {
                kinds[continued] = Line::Continued;
            }
        }
        find_guard();
        std::uint32_t depth = 0;
        for(std::uint32_t line = 0; line < count; line += 1) {
            auto keyword = keywords[line];
            if(kinds[line] == Line::Directive && keyword == "endif" && depth != 0) {
                depth -= 1;
            }
            depths.push_back(depth);
            if(kinds[line] == Line::Directive &&
               (keyword == "if" || keyword == "ifdef" || keyword == "ifndef")) {
                depth += 1;
            }
        }
    }

    std::uint32_t line_of(std::uint32_t offset) const {
        return static_cast<std::uint32_t>(std::ranges::upper_bound(starts, offset) -
                                          starts.begin() - 1);
    }

    bool pragma_once(std::uint32_t line) const {
        return kinds[line] == Line::Directive && keywords[line] == "pragma" &&
               !arguments[line].empty() && arguments[line].front() == "once";
    }

    /// `#ifndef X` (or `#if !defined(X)`) and a bare `#define X` first, the
    /// `#endif` closing the first last, no `#else` between.
    void find_guard() {
        llvm::SmallVector<std::uint32_t> filled;
        for(std::uint32_t line = 0; line < lines.size(); line += 1) {
            if(kinds[line] != Line::Blank && !pragma_once(line)) {
                filled.push_back(line);
            }
        }
        if(filled.size() < 3) {
            return;
        }
        auto opening = filled[0];
        auto definition = filled[1];
        if(kinds[opening] != Line::Directive || kinds[definition] != Line::Directive ||
           keywords[definition] != "define" || arguments[definition].size() != 1) {
            return;
        }
        auto& tested = arguments[opening];
        auto name = arguments[definition].front();
        auto negated = [&](llvm::ArrayRef<llvm::StringRef> rest) {
            return rest.size() >= 3 && rest[0] == "!" && rest[1] == "defined" &&
                   (rest.size() == 3
                        ? rest[2] == name
                        : rest.size() == 5 && rest[2] == "(" && rest[3] == name && rest[4] == ")");
        };
        if(!(keywords[opening] == "ifndef" && tested.size() == 1 && tested.front() == name) &&
           !(keywords[opening] == "if" && negated(tested))) {
            return;
        }
        int depth = 0;
        for(auto line: filled) {
            if(kinds[line] != Line::Directive) {
                continue;
            }
            auto keyword = keywords[line];
            if(depth == 1 && keyword.starts_with("el")) {
                return;
            }
            depth += keyword == "if" || keyword == "ifdef" || keyword == "ifndef";
            depth -= keyword == "endif";
            if(depth == 0) {
                if(line == filled.back()) {
                    guard = {opening, definition, line};
                    for(auto member: guard) {
                        kinds[member] = Line::Blank;
                    }
                }
                return;
            }
        }
    }

    /// The first line of the body: past the leading directives, before a
    /// conditional block the body continues; the comments after the last of
    /// them document the body.
    std::uint32_t preamble_end() const {
        llvm::SmallVector<std::uint32_t> opened;
        std::uint32_t last = 0;
        for(std::uint32_t line = 0; line < lines.size(); line += 1) {
            if(kinds[line] == Line::Code) {
                break;
            }
            if(kinds[line] == Line::Directive) {
                auto keyword = keywords[line];
                if(keyword == "if" || keyword == "ifdef" || keyword == "ifndef") {
                    opened.push_back(line);
                } else if(keyword == "endif" && !opened.empty()) {
                    opened.pop_back();
                }
                last = line + 1;
            } else if(kinds[line] == Line::Continued) {
                last = line + 1;
            }
        }
        return opened.empty() ? last : opened.front();
    }

    /// The tokens on `line`.
    llvm::ArrayRef<Token> tokens_on(std::uint32_t line) const {
        auto begin = std::ranges::lower_bound(token_lines, line) - token_lines.begin();
        auto end = std::ranges::upper_bound(token_lines, line) - token_lines.begin();
        return llvm::ArrayRef(tokens).slice(begin, end - begin);
    }

    /// The tokens on lines `first` through `last`.
    llvm::ArrayRef<Token> tokens_on(std::uint32_t first, std::uint32_t last) const {
        auto begin = std::ranges::lower_bound(token_lines, first) - token_lines.begin();
        auto end = std::ranges::upper_bound(token_lines, last) - token_lines.begin();
        return llvm::ArrayRef(tokens).slice(begin, end - begin);
    }

    /// The tokens on lines `first` through `last`, spaced: the code without
    /// its comments.
    std::string code(std::uint32_t first, std::uint32_t last) const {
        std::string text;
        for(auto& token: tokens_on(first, last)) {
            if(!text.empty()) {
                text += ' ';
            }
            text += token.text(content);
        }
        return text;
    }

    /// The tokens past a leading template head `template <...>`, all of
    /// them when there is none; nothing when the head does not close.
    std::optional<llvm::ArrayRef<Token>> past_template_head(llvm::ArrayRef<Token> rest) const {
        if(rest.empty() || rest.front().text(content) != "template") {
            return rest;
        }
        int depth = 0;
        for(std::size_t i = 1; i < rest.size(); i += 1) {
            depth += rest[i].kind == clang::tok::less;
            depth -= rest[i].kind == clang::tok::greater;
            depth -= 2 * (rest[i].kind == clang::tok::greatergreater);
            if(depth <= 0) {
                return rest.drop_front(i + 1);
            }
        }
        return std::nullopt;
    }

    /// The lines of the forward declaration of a type on `line`, when they
    /// hold it and nothing else: `class C;`, `enum class E : int;`, and
    /// `template <typename T> struct S;` on one line or with the template
    /// head (and a requires-clause) on the lines before.
    std::optional<std::pair<std::uint32_t, std::uint32_t>>
        forward_declaration(std::uint32_t line) const {
        auto all = tokens_on(line);
        auto past = past_template_head(all);
        if(!past) {
            return std::nullopt;
        }
        auto rest = *past;
        auto text = [&](std::size_t i) {
            return i < rest.size() ? rest[i].text(content) : llvm::StringRef();
        };
        std::size_t i = 0;
        if(text(i) == "enum") {
            i += 1;
            if(text(i) == "class" || text(i) == "struct") {
                i += 1;
            }
        } else if(text(i) == "class" || text(i) == "struct" || text(i) == "union") {
            i += 1;
        } else {
            return std::nullopt;
        }
        if(i >= rest.size() || !rest[i].is_identifier()) {
            return std::nullopt;
        }
        i += 1;
        if(i < rest.size() && rest[i].kind == clang::tok::colon) {
            for(i += 1; i < rest.size() &&
                        (rest[i].is_identifier() || rest[i].kind == clang::tok::coloncolon);
                i += 1) {}
        }
        if(i + 1 != rest.size() || rest[i].kind != clang::tok::semi) {
            return std::nullopt;
        }
        auto first = line;
        // The lines above that end no statement or block lead up to it.
        for(auto previous = line; rest.size() == all.size() && previous != 0;) {
            previous -= 1;
            if(kinds[previous] == Line::Blank) {
                continue;
            }
            auto above = tokens_on(previous);
            if(kinds[previous] != Line::Code || above.back().kind == clang::tok::semi ||
               above.back().kind == clang::tok::l_brace ||
               above.back().kind == clang::tok::r_brace) {
                break;
            }
            if(above.front().text(content) == "template") {
                if(past_template_head(tokens_on(previous, line - 1))) {
                    first = previous;
                }
                break;
            }
        }
        return std::pair{first, line};
    }

    /// The opening and closing lines of each anonymous namespace standing on
    /// lines of their own.
    llvm::DenseSet<std::uint32_t> anonymous_namespaces() const {
        llvm::DenseSet<std::uint32_t> found;
        for(std::size_t i = 0; i + 1 < tokens.size(); i += 1) {
            auto line = token_lines[i];
            if(tokens[i].text(content) != "namespace" ||
               tokens[i + 1].kind != clang::tok::l_brace || tokens_on(line).size() != 2) {
                continue;
            }
            int depth = 0;
            for(auto close = i + 1; close < tokens.size(); close += 1) {
                depth += tokens[close].kind == clang::tok::l_brace;
                depth -= tokens[close].kind == clang::tok::r_brace;
                if(depth == 0) {
                    if(tokens_on(token_lines[close]).size() == 1) {
                        found.insert(line);
                        found.insert(token_lines[close]);
                    }
                    break;
                }
            }
        }
        return found;
    }

    /// The offset of `int` in a definition of `main` at global scope, the
    /// last one: `int main(` in a string literal is no token.
    std::optional<std::uint32_t> main_definition() const {
        std::optional<std::uint32_t> found;
        int depth = 0;
        for(std::size_t i = 0; i < tokens.size(); i += 1) {
            depth += tokens[i].kind == clang::tok::l_brace;
            depth -= tokens[i].kind == clang::tok::r_brace;
            if(depth == 0 && i + 2 < tokens.size() && tokens[i].text(content) == "int" &&
               tokens[i + 1].text(content) == "main" && tokens[i + 2].kind == clang::tok::l_paren) {
                found = tokens[i].range.begin;
            }
        }
        return found;
    }
};

bool is_keyword(llvm::StringRef word) {
    static clang::IdentifierTable table(cxx());
    return table.get(word).getTokenID() != clang::tok::identifier;
}

/// A partition name for the file at `path`: its path under `base` when it is
/// there, else its whole path, without the extension, each segment made an
/// identifier.
std::string partition_name(llvm::StringRef path, llvm::StringRef base) {
    llvm::StringRef relative = path;
    if(!base.empty() && path.size() > base.size() && path::under(path, base)) {
        relative = path.drop_front(base.size() + 1);
    }
    auto extension = llvm::sys::path::extension(relative, posix);
    relative = relative.drop_back(extension.size());
    llvm::SmallVector<llvm::StringRef> segments;
    relative.split(segments, '/');
    llvm::SmallVector<std::string> names;
    for(auto segment: segments) {
        std::string name;
        for(auto c: segment) {
            name += llvm::isAlnum(c) || c == '_' ? c : '_';
        }
        if(name.empty() || llvm::isDigit(name.front())) {
            name.insert(name.begin(), '_');
        }
        if(is_keyword(name)) {
            name += '_';
        }
        names.push_back(std::move(name));
    }
    return llvm::join(names, ".");
}

std::string with_extension(llvm::StringRef path, llvm::StringRef extension) {
    llvm::SmallString<256> result(path);
    llvm::sys::path::replace_extension(result, extension, posix);
    return result.str().str();
}

/// The text of `lines` joined, runs of blank lines folded to one, ending
/// in a single newline.
std::string assemble(llvm::ArrayRef<std::string> lines) {
    std::string text;
    std::uint32_t blanks = 0;
    for(auto& line: lines) {
        if(llvm::StringRef(line).trim().empty() && line.find('\r') == std::string::npos) {
            blanks += 1;
            if(blanks > 1 || text.empty()) {
                continue;
            }
        } else {
            blanks = 0;
        }
        text += line;
        text += '\n';
    }
    while(llvm::StringRef(text).ends_with("\n\n")) {
        text.pop_back();
    }
    return text;
}

enum class Action : std::uint8_t {
    /// The directive stays.
    Keep,
    /// The prelude imports what it includes.
    Drop,
    /// It names a partition of the includer's module.
    Partition,
    /// It names a header of another rewritten module.
    Import,
};

struct Rewriter {
    const Facts& facts;
    const Partition& partition;
    llvm::ArrayRef<Unit> units;

    /// Headers of wrapped modules that stay textual where they are included.
    llvm::DenseSet<std::uint32_t> kept;

    /// Include operand -> the one rewritten header includers name by it,
    /// for a directive the index did not see, in a branch this configuration
    /// skips: that include still has to become an import. Any other such
    /// directive stays as written.
    llvm::StringMap<std::uint32_t> spelled;

    /// Directory -> how includes name files under it as an include root:
    /// `<` when an angle spelling does, else `"`, which may come from
    /// -iquote.
    llvm::StringMap<char> roots;

    bool rewritten(std::uint32_t file) const {
        return !partition.primaries[partition.module_of[file]].empty() &&
               units[file].kind != Unit::Kind::Fragment;
    }

    bool header(std::uint32_t file) const {
        return units[file].kind == Unit::Kind::Internal ||
               units[file].kind == Unit::Kind::Interface;
    }

    /// A header that stays a header the files including it read: one of a
    /// module that stays headers, or a wrapped module's textual one.
    bool textual(std::uint32_t file) const {
        auto module = partition.module_of[file];
        return header(file) && partition.primaries[module].empty() &&
               (partition.kinds[module] == ModuleKind::Program || kept.contains(file));
    }

    std::string partition_of(std::uint32_t file) const {
        auto& primary = partition.primaries[partition.module_of[file]];
        return partition_name(facts.files[file].path, llvm::sys::path::parent_path(primary, posix));
    }

    /// How `user` names the file: the most common spelling of its includers
    /// that resolves from there. `"a.h"` from beside the file resolves only
    /// beside it; a spelling used from another directory goes through the
    /// include path. With none, its path under the deepest include root,
    /// else its workspace-relative path, the root being on the include path
    /// as for the prelude.
    std::string spelling(std::uint32_t file, std::uint32_t user) const {
        auto& info = facts.files[file];
        auto directory = llvm::sys::path::parent_path(info.path, posix);
        auto beside = [&](llvm::StringRef from, llvm::StringRef spelled) {
            llvm::SmallString<256> path(llvm::sys::path::parent_path(from, posix));
            llvm::sys::path::append(path, posix, spelled.drop_front().drop_back());
            llvm::sys::path::remove_dots(path, true, posix);
            return path.str() == info.path;
        };
        std::map<llvm::StringRef, std::uint32_t> counts;
        for(std::size_t i = 0; i < info.includers.size(); i += 1) {
            llvm::StringRef spelled = info.spellings[i];
            auto& includer = facts.files[info.includers[i]].path;
            if(!spelled.empty() &&
               (spelled.starts_with("<") || beside(facts.files[user].path, spelled) ||
                llvm::sys::path::parent_path(includer, posix) != directory)) {
                counts[spelled] += 1;
            }
        }
        auto best = std::ranges::max_element(counts, {}, [](auto& entry) { return entry.second; });
        if(best != counts.end()) {
            return best->first.str();
        }
        const llvm::StringMapEntry<char>* deepest = nullptr;
        for(auto& root: roots) {
            if(path::under(info.path, root.first()) &&
               (!deepest || root.first().size() > deepest->first().size())) {
                deepest = &root;
            }
        }
        if(!deepest) {
            return std::format(R"("{}")", info.path);
        }
        auto relative = llvm::StringRef(info.path).drop_front(deepest->first().size() + 1);
        return deepest->second == '<' ? std::format("<{}>", relative.str())
                                      : std::format(R"("{}")", relative.str());
    }

    /// The file the include directive on `line` names. A forced include's
    /// presumed line may coincide with a directive's; the operand's file name
    /// tells them apart.
    std::uint32_t resolve(std::uint32_t file, std::uint32_t line, llvm::StringRef operand) const {
        auto [begin, end] =
            std::ranges::equal_range(facts.files[file].directives,
                                     line + 1,
                                     {},
                                     &std::pair<std::uint32_t, std::uint32_t>::first);
        if(end - begin > 1 && operand.size() > 2) {
            auto name = llvm::sys::path::filename(operand.drop_front().drop_back(), posix);
            for(auto it = begin; it != end; ++it) {
                if(llvm::sys::path::filename(facts.files[it->second].path, posix) == name) {
                    return it->second;
                }
            }
        }
        if(begin != end) {
            return begin->second;
        }
        auto found = spelled.find(operand);
        return found == spelled.end() ? none : found->second;
    }

    Action classify(std::uint32_t from, std::uint32_t target) const {
        if(target == none || units[target].kind == Unit::Kind::Fragment) {
            return Action::Keep;
        }
        auto module = partition.module_of[target];
        if(rewritten(target)) {
            if(!header(target)) {
                return Action::Keep;
            }
            if(module != partition.module_of[from]) {
                return Action::Import;
            }
            // A source sees its module's interface partitions through the
            // primary interface every implementation unit imports.
            return header(from) || units[target].kind == Unit::Kind::Internal ? Action::Partition
                                                                              : Action::Drop;
        }
        auto kind = partition.kinds[module];
        if((kind == ModuleKind::Wrapped || kind == ModuleKind::External) &&
           !kept.contains(target)) {
            return Action::Drop;
        }
        return Action::Keep;
    }
};

/// What rewriting one file found, before the partitions it reaches are
/// known.
struct Draft {
    std::vector<std::string> fragment;
    std::vector<std::string> body;
    std::set<std::string> modules;
    llvm::DenseSet<std::uint32_t> partitions;

    /// Headers that stay headers, to include: the ones it names or takes
    /// macros from without including them, and those of the partitions it
    /// reaches only through another partition.
    std::set<std::uint32_t> needed;

    /// The headers that stay headers it includes itself, and the lines doing
    /// so.
    std::set<std::uint32_t> textual;
    llvm::DenseSet<std::uint32_t> textual_lines;

    std::set<std::uint32_t> macro_headers;
};

}  // namespace

std::expected<Rewriting, std::string> rewrite(const Facts& facts,
                                              Partition partition,
                                              llvm::ArrayRef<Interface> interfaces,
                                              llvm::StringRef prelude,
                                              llvm::StringRef root) {
    Rewriting result;
    auto& plan = result.plan;

    // A source defining what a header of a rewritten module declares joins
    // that module: a definition is attached to the module of its
    // declaration.
    std::map<std::uint32_t, std::set<std::uint32_t>> joins;
    for(auto& redeclaration: facts.redeclarations) {
        auto owner = facts.entities[redeclaration.entity].owner;
        auto module = partition.module_of[owner];
        if(redeclaration.definition && facts.files[redeclaration.file].source &&
           !facts.files[owner].source && !partition.primaries[module].empty() &&
           partition.module_of[redeclaration.file] != module) {
            joins[redeclaration.file].insert(module);
        }
    }
    for(auto& [file, modules]: joins) {
        auto& path = facts.files[file].path;
        if(modules.size() > 1) {
            plan.warnings.push_back(
                std::format("{} defines what headers of {} modules declare", path, modules.size()));
            continue;
        }
        partition.module_of[file] = *modules.begin();
        plan.moved.push_back(std::format("{}={}", path, partition.modules[*modules.begin()]));
    }

    Annotations annotations;
    auto units = Report{.facts = facts, .partition = partition, .annotations = annotations}.units();
    Rewriter rewriter{.facts = facts, .partition = partition, .units = units};
    for(std::uint32_t module = 0; module < interfaces.size(); module += 1) {
        if(partition.kinds[module] != ModuleKind::Wrapped) {
            continue;
        }
        for(auto& header: interfaces[module].textual) {
            rewriter.kept.insert(facts.file_ids.lookup(header.file));
        }
    }
    llvm::StringSet<> ambiguous;
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        if(!rewriter.rewritten(file) || !rewriter.header(file)) {
            continue;
        }
        for(auto& spelled: facts.files[file].spellings) {
            if(spelled.empty() || ambiguous.contains(spelled)) {
                continue;
            }
            auto [it, inserted] = rewriter.spelled.try_emplace(spelled, file);
            if(!inserted && it->second != file) {
                rewriter.spelled.erase(it);
                ambiguous.insert(spelled);
            }
        }
    }
    for(auto& info: facts.files) {
        for(std::size_t i = 0; i < info.includers.size(); i += 1) {
            llvm::StringRef spelled = info.spellings[i];
            llvm::StringRef path = info.path;
            if(spelled.size() < 3) {
                continue;
            }
            auto relative = spelled.drop_front().drop_back();
            if(!path.ends_with(("/" + relative).str())) {
                continue;
            }
            auto root = path.drop_back(relative.size() + 1);
            if(root.empty() ||
               (spelled.front() == '"' &&
                root == llvm::sys::path::parent_path(facts.files[info.includers[i]].path, posix))) {
                continue;
            }
            auto& delimiter = rewriter.roots[root];
            if(delimiter != '<') {
                delimiter = spelled.front();
            }
        }
    }

    std::vector<std::uint32_t> files;
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        auto& info = facts.files[file];
        if(rewriter.rewritten(file)) {
            files.push_back(file);
            continue;
        }
        auto includes_rewritten = llvm::any_of(info.includes, [&](std::uint32_t included) {
            return rewriter.rewritten(included) && rewriter.header(included);
        });
        if(!includes_rewritten || units[file].kind == Unit::Kind::Fragment) {
            continue;
        }
        if(info.source) {
            files.push_back(file);
        } else {
            plan.warnings.push_back(
                std::format("{} stays a header but includes rewritten headers", info.path));
        }
    }

    llvm::DenseMap<std::uint32_t, std::unique_ptr<llvm::MemoryBuffer>> buffers;
    std::map<std::uint32_t, Draft> drafts;
    for(auto file: files) {
        auto& info = facts.files[file];
        auto path = path::join(root, info.path);
        auto buffer = vfs::read(path);
        if(!buffer) {
            return std::unexpected(
                std::format("cannot read {}: {}", path, buffer.error().message()));
        }
        auto& draft = drafts[file];
        Text text((*buffer)->getBuffer());
        buffers[file] = std::move(*buffer);
        auto module_unit = rewriter.rewritten(file);

        // Declarations of other modules' entities: in a module unit they
        // would declare a second entity, attached to its module. One whose
        // entity lies outside the scope moves to the global module fragment.
        llvm::DenseSet<std::uint32_t> dropped(text.guard.begin(), text.guard.end());
        if(module_unit) {
            auto drop = [&](std::uint32_t line) -> std::optional<std::string> {
                if(line == 0 || line > text.lines.size()) {
                    return std::nullopt;
                }
                auto span = text.forward_declaration(line - 1);
                if(!span) {
                    return std::nullopt;
                }
                for(auto member = span->first; member <= span->second; member += 1) {
                    dropped.insert(member);
                }
                return text.code(span->first, span->second);
            };
            for(auto& redeclaration: facts.redeclarations) {
                auto owner = facts.entities[redeclaration.entity].owner;
                if(redeclaration.file == file && !redeclaration.definition &&
                   !redeclaration.friend_declaration &&
                   partition.module_of[owner] != partition.module_of[file] &&
                   !drop(redeclaration.line)) {
                    plan.warnings.push_back(
                        std::format("{}:{} declares {} of module {}, which only that module may",
                                    info.path,
                                    redeclaration.line,
                                    facts.entities[redeclaration.entity].name,
                                    partition.modules[partition.module_of[owner]]));
                }
            }
            for(auto& foreign: facts.foreign_declarations) {
                if(foreign.file != file || foreign.friend_declaration) {
                    continue;
                }
                if(auto declaration = drop(foreign.line)) {
                    auto [scope, name] = llvm::StringRef(foreign.name).rsplit("::");
                    draft.fragment.push_back(
                        name.empty() ? *declaration
                                     : std::format("namespace {} {{ {} }}", scope, *declaration));
                }
            }
            if(rewriter.header(file)) {
                for(auto line: text.anonymous_namespaces()) {
                    dropped.insert(line);
                }
            }
        }

        std::optional<std::uint32_t> main;
        if(module_unit && !rewriter.header(file)) {
            main = text.main_definition();
        }
        auto end = text.preamble_end();
        for(std::uint32_t line = 0; line < text.lines.size(); line += 1) {
            auto& out = line < end ? draft.fragment : draft.body;
            auto keyword = text.keywords[line];
            if(text.pragma_once(line)) {
                continue;
            }
            if(text.kinds[line] == Text::Line::Directive && takes_header_name(keyword) &&
               keyword != "embed") {
                auto target = rewriter.resolve(file, line, text.operands[line]);
                switch(rewriter.classify(file, target)) {
                    case Action::Import:
                        draft.modules.insert(partition.modules[partition.module_of[target]]);
                        continue;
                    case Action::Partition: draft.partitions.insert(target); continue;
                    case Action::Drop: continue;
                    case Action::Keep:
                        if(target == none || !rewriter.textual(target)) {
                            break;
                        }
                        draft.textual.insert(target);
                        draft.textual_lines.insert(line);
                        // In the purview its declarations would be attached to
                        // the module.
                        if(module_unit && line >= end) {
                            if(text.depths[line] != 0) {
                                plan.warnings.push_back(std::format(
                                    "{}:{} includes {} under a condition in the module purview",
                                    info.path,
                                    line + 1,
                                    facts.files[target].path));
                                break;
                            }
                            draft.fragment.push_back(text.lines[line].str());
                            continue;
                        }
                        break;
                }
            }
            if(dropped.contains(line)) {
                continue;
            }
            auto text_line = text.lines[line].str();
            if(main && text.line_of(*main) == line) {
                text_line.insert(*main - text.starts[line], R"(extern "C++" )");
            }
            out.push_back(std::move(text_line));
        }

        for(auto named: units[file].names) {
            auto action = rewriter.classify(file, named);
            if(action == Action::Import) {
                draft.modules.insert(partition.modules[partition.module_of[named]]);
            } else if(action == Action::Keep && rewriter.textual(named) &&
                      !llvm::is_contained(info.includes, named)) {
                draft.needed.insert(named);
            }
        }
        // Imports carry no macros: a rewritten header's reach its users
        // through its macro header, a textual header's through an include.
        for(auto defining: units[file].macros) {
            if(rewriter.rewritten(defining) && rewriter.header(defining)) {
                draft.macro_headers.insert(defining);
            } else if(rewriter.textual(defining) && !llvm::is_contained(info.includes, defining)) {
                draft.needed.insert(defining);
            }
        }
        draft.partitions.erase(file);
    }

    // Clang takes no declaration in the global module fragment of an
    // implementation partition as reachable from a unit importing it through
    // another partition, though the standard has that unit import it too:
    // the unit includes the textual headers such a partition does itself.
    for(auto& [file, draft]: drafts) {
        if(!rewriter.rewritten(file)) {
            continue;
        }
        llvm::DenseSet<std::uint32_t> reached(draft.partitions.begin(), draft.partitions.end());
        llvm::SmallVector<std::uint32_t> pending(draft.partitions.begin(), draft.partitions.end());
        while(!pending.empty()) {
            auto& through = drafts.at(pending.pop_back_val());
            for(auto next: through.partitions) {
                if(!reached.insert(next).second) {
                    continue;
                }
                pending.push_back(next);
                if(units[next].kind == Unit::Kind::Internal) {
                    for(auto header: drafts.at(next).textual) {
                        if(!llvm::is_contained(facts.files[file].includes, header)) {
                            draft.needed.insert(header);
                        }
                    }
                }
            }
        }
    }

    llvm::StringSet<> primaries;
    for(auto& primary: partition.primaries) {
        if(!primary.empty()) {
            primaries.insert(primary);
        }
    }

    // The macro header of `defining` named the way `user` names the header,
    // `"support/logging.h"` giving `"support/logging.macros.h"`.
    auto macro_include = [&](std::uint32_t defining, std::uint32_t user) {
        auto spelling = rewriter.spelling(defining, user);
        llvm::StringRef spelled = spelling;
        auto named = with_extension(spelled.drop_front().drop_back(), "macros.h");
        auto [open, close] = spelled.starts_with("<") ? std::pair{'<', '>'} : std::pair{'"', '"'};
        return std::format("#include {}{}{}", open, named, close);
    };

    std::map<std::uint32_t, Rewriting::Module> modules;
    std::set<std::uint32_t> macro_headers;
    for(auto& [file, draft]: drafts) {
        auto& info = facts.files[file];
        auto module = partition.module_of[file];
        std::vector<std::string> lines;
        auto module_unit = rewriter.rewritten(file);
        if(module_unit) {
            lines.push_back("module;");
            lines.emplace_back();
        }
        lines.push_back(std::format(R"(#include "{}")", prelude.str()));
        llvm::append_range(lines, draft.fragment);
        for(auto needed: draft.needed) {
            lines.push_back(std::format("#include {}", rewriter.spelling(needed, file)));
        }
        std::set<std::string> macro_includes;
        for(auto defining: draft.macro_headers) {
            macro_headers.insert(defining);
            macro_includes.insert(macro_include(defining, file));
        }
        llvm::append_range(lines, macro_includes);

        auto interface = module_unit && units[file].kind == Unit::Kind::Interface;
        if(module_unit) {
            lines.emplace_back();
            auto& name = partition.modules[module];
            if(rewriter.header(file)) {
                lines.push_back(std::format("{}module {}:{};",
                                            interface ? "export " : "",
                                            name,
                                            rewriter.partition_of(file)));
            } else {
                lines.push_back(std::format("module {};", name));
            }
            lines.emplace_back();
        }
        for(auto& imported: draft.modules) {
            lines.push_back(std::format("import {};", imported));
        }
        std::set<std::string> partitions;
        for(auto imported: draft.partitions) {
            partitions.insert(rewriter.partition_of(imported));
        }
        for(auto& name: partitions) {
            lines.push_back(std::format("import :{};", name));
        }
        if(!draft.modules.empty() || !partitions.empty()) {
            lines.emplace_back();
        }
        if(interface) {
            lines.push_back("export {");
        }
        llvm::append_range(lines, draft.body);
        if(interface) {
            lines.push_back("}");
        }

        auto path =
            module_unit && rewriter.header(file) ? with_extension(info.path, "cppm") : info.path;
        if(primaries.contains(path)) {
            return std::unexpected(
                std::format("{} becomes {}, the primary interface of its module", info.path, path));
        }
        result.files.push_back({.path = path, .content = assemble(lines)});
        if(!module_unit) {
            plan.importers.push_back(path);
            continue;
        }
        auto& entry = modules[module];
        if(rewriter.header(file)) {
            plan.removed.push_back(info.path);
            (interface ? entry.interfaces : entry.partitions).push_back(path);
        } else {
            entry.sources.push_back(path);
        }
        for(auto& imported: draft.modules) {
            if(!llvm::is_contained(entry.imports, imported)) {
                entry.imports.push_back(imported);
            }
        }
    }

    for(auto& [module, entry]: modules) {
        entry.name = partition.modules[module];
        entry.primary = partition.primaries[module];
        std::ranges::sort(entry.interfaces);
        std::ranges::sort(entry.partitions);
        std::ranges::sort(entry.sources);
        llvm::StringMap<std::string> names;
        std::vector<std::string> lines{std::format("export module {};", entry.name), ""};
        for(auto* group: {&entry.interfaces, &entry.partitions}) {
            for(auto& path: *group) {
                auto name =
                    partition_name(path, llvm::sys::path::parent_path(entry.primary, posix));
                if(auto [it, inserted] = names.try_emplace(name, path); !inserted) {
                    return std::unexpected(std::format("{} and {} are both partition {} of {}",
                                                       it->second,
                                                       path,
                                                       name,
                                                       entry.name));
                }
                if(group == &entry.interfaces) {
                    lines.push_back(std::format("export import :{};", name));
                }
            }
        }
        result.files.push_back({.path = entry.primary, .content = assemble(lines)});
        std::ranges::sort(entry.imports);
        plan.modules.push_back(std::move(entry));
    }

    // A macro header replays the header's directives, the includes of the
    // headers that stay headers among them, after the macro headers of the
    // rewritten headers whose macros it uses: its conditions may test them.
    for(auto defining: macro_headers) {
        Text text(buffers[defining]->getBuffer());
        auto& draft = drafts.at(defining);
        auto& textual_lines = draft.textual_lines;
        std::vector<std::string> lines{"#pragma once", ""};
        for(auto used: draft.macro_headers) {
            lines.push_back(macro_include(used, defining));
        }
        for(std::uint32_t line = 0; line < text.lines.size(); line += 1) {
            auto keyword = text.keywords[line];
            if(text.kinds[line] != Text::Line::Directive || keyword == "pragma" ||
               (takes_header_name(keyword) && !textual_lines.contains(line))) {
                continue;
            }
            for(auto part = line; part <= text.ends[line]; part += 1) {
                lines.push_back(text.lines[part].str());
            }
        }
        auto path = with_extension(facts.files[defining].path, "macros.h");
        result.files.push_back({.path = path, .content = assemble(lines)});
        plan.macros.push_back(std::move(path));
    }
    for(auto* list: {&plan.importers, &plan.macros, &plan.removed, &plan.warnings}) {
        std::ranges::sort(*list);
    }
    plan.warnings.erase(std::ranges::unique(plan.warnings).begin(), plan.warnings.end());
    return result;
}

}  // namespace clice::analysis
