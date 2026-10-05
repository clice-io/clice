#include "analysis/rewriting.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <utility>

#include "syntax/lexer.h"
#include "vfs/file_system.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

namespace clice::analysis {

namespace {

constexpr std::uint32_t none = ~0u;

constexpr auto posix = llvm::sys::path::Style::posix;

/// A file's lines and what the lexer finds on each.
struct Text {
    enum class Line : std::uint8_t {
        /// Whitespace and comments.
        Blank,
        Code,
        /// The first line of a preprocessor directive.
        Directive,
        /// A directive's continuation line.
        Continued,
    };

    std::vector<llvm::StringRef> lines;
    std::vector<std::uint32_t> starts;
    std::vector<Line> kinds;

    /// Per line: the keyword of the directive it starts, and an include's
    /// operand as written, `<...>` or `"..."`.
    std::vector<llvm::StringRef> keywords;
    std::vector<llvm::StringRef> operands;

    /// Per directive: its last line.
    std::vector<std::uint32_t> ends;

    /// The tokens outside directives and the line of each.
    std::vector<Token> tokens;
    std::vector<std::uint32_t> token_lines;

    llvm::StringRef content;

    /// `content` ends at a NUL terminator, as the lexer requires.
    explicit Text(llvm::StringRef content) : content(content) {
        llvm::SmallVector<llvm::StringRef> parts;
        content.split(parts, '\n');
        // The newline ending the last line starts none.
        if(content.ends_with("\n")) {
            parts.pop_back();
        }
        lines.assign(parts.begin(), parts.end());
        std::uint32_t offset = 0;
        for(auto line: lines) {
            starts.push_back(offset);
            offset += static_cast<std::uint32_t>(line.size()) + 1;
        }
        auto count = lines.size();
        kinds.assign(count, Line::Blank);
        keywords.resize(count);
        operands.resize(count);
        ends.resize(count);

        const static auto options =
            raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx23);
        Lexer lexer(content, {.lang_opts = &options});
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
                }
            }
            ends[line] = last;
            for(auto continued = line + 1; continued <= last; continued += 1) {
                kinds[continued] = Line::Continued;
            }
        }
    }

    std::uint32_t line_of(std::uint32_t offset) const {
        return static_cast<std::uint32_t>(std::ranges::upper_bound(starts, offset) -
                                          starts.begin() - 1);
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

    /// Whether `line` holds one forward declaration of a type and nothing
    /// else: `class C;`, `template <typename T> struct S;`, `enum class E : int;`.
    bool forward_declaration(std::uint32_t line) const {
        auto rest = tokens_on(line);
        auto text = [&](std::size_t i) {
            return i < rest.size() ? rest[i].text(content) : llvm::StringRef();
        };
        std::size_t i = 0;
        if(text(i) == "template") {
            i += 1;
            int depth = 0;
            for(; i < rest.size(); i += 1) {
                depth += rest[i].kind == clang::tok::less;
                depth -= rest[i].kind == clang::tok::greater;
                depth -= 2 * (rest[i].kind == clang::tok::greatergreater);
                if(depth <= 0) {
                    break;
                }
            }
            i += 1;
        }
        if(text(i) == "enum") {
            i += 1;
            if(text(i) == "class" || text(i) == "struct") {
                i += 1;
            }
        } else if(text(i) == "class" || text(i) == "struct" || text(i) == "union") {
            i += 1;
        } else {
            return false;
        }
        if(i >= rest.size() || !rest[i].is_identifier()) {
            return false;
        }
        i += 1;
        if(i < rest.size() && rest[i].kind == clang::tok::colon) {
            for(i += 1; i < rest.size() &&
                        (rest[i].is_identifier() || rest[i].kind == clang::tok::coloncolon);
                i += 1) {}
        }
        return i + 1 == rest.size() && rest[i].kind == clang::tok::semi;
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
    const static llvm::StringSet<> keywords = {
        "alignas",       "alignof",     "and",
        "and_eq",        "asm",         "auto",
        "bitand",        "bitor",       "bool",
        "break",         "case",        "catch",
        "char",          "char8_t",     "char16_t",
        "char32_t",      "class",       "compl",
        "concept",       "const",       "consteval",
        "constexpr",     "constinit",   "const_cast",
        "continue",      "co_await",    "co_return",
        "co_yield",      "decltype",    "default",
        "delete",        "do",          "double",
        "dynamic_cast",  "else",        "enum",
        "explicit",      "export",      "extern",
        "false",         "float",       "for",
        "friend",        "goto",        "if",
        "inline",        "int",         "long",
        "mutable",       "namespace",   "new",
        "noexcept",      "not",         "not_eq",
        "nullptr",       "operator",    "or",
        "or_eq",         "private",     "protected",
        "public",        "register",    "reinterpret_cast",
        "requires",      "return",      "short",
        "signed",        "sizeof",      "static",
        "static_assert", "static_cast", "struct",
        "switch",        "template",    "this",
        "thread_local",  "throw",       "true",
        "try",           "typedef",     "typeid",
        "typename",      "union",       "unsigned",
        "using",         "virtual",     "void",
        "volatile",      "wchar_t",     "while",
        "xor",           "xor_eq",
    };
    return keywords.contains(word);
}

/// A partition name for the file at `path`: its path under `base` when it is
/// there, else its whole path, without the extension, each segment made an
/// identifier.
std::string partition_name(llvm::StringRef path, llvm::StringRef base) {
    llvm::StringRef relative = path;
    if(!base.empty() && path.starts_with(base) && path.size() > base.size() &&
       path[base.size()] == '/') {
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

    /// Include operand -> the one scoped file includers name by it, for a
    /// directive the index did not see: a branch this configuration skips.
    llvm::StringMap<std::uint32_t> spelled;

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

    /// How includers name the file, the most common spelling.
    std::string spelling(std::uint32_t file) const {
        std::map<llvm::StringRef, std::uint32_t> counts;
        for(auto& spelled: facts.files[file].spellings) {
            if(!spelled.empty()) {
                counts[spelled] += 1;
            }
        }
        auto best = std::ranges::max_element(counts, {}, [](auto& entry) { return entry.second; });
        return best == counts.end() ? std::string() : best->first.str();
    }

    std::uint32_t resolve(std::uint32_t file, std::uint32_t line, llvm::StringRef operand) const {
        auto& directives = facts.files[file].directives;
        auto it = std::ranges::lower_bound(directives, std::pair{line + 1, 0u});
        if(it != directives.end() && it->first == line + 1) {
            return it->second;
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

    /// Headers that stay headers, to include: the ones it names without
    /// including them, and those of the partitions it reaches only through
    /// another partition.
    std::set<std::uint32_t> needed;

    /// The headers that stay headers it includes itself.
    std::set<std::uint32_t> textual;
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
    auto is_rewritten_module = [&](std::uint32_t module) {
        return !partition.primaries[module].empty();
    };

    // A source defining what a header of a rewritten module declares joins
    // that module: a definition is attached to the module of its
    // declaration.
    std::map<std::uint32_t, std::set<std::uint32_t>> joins;
    for(auto& redeclaration: facts.redeclarations) {
        auto owner = facts.entities[redeclaration.entity].owner;
        auto module = partition.module_of[owner];
        if(redeclaration.definition && facts.files[redeclaration.file].source &&
           !facts.files[owner].source && is_rewritten_module(module) &&
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
            if(auto it = facts.file_ids.find(header.file); it != facts.file_ids.end()) {
                rewriter.kept.insert(it->second);
            }
        }
    }
    llvm::StringSet<> ambiguous;
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
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

    // The files to rewrite: every file of a rewritten module, and the
    // sources of other modules including one of its headers.
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

    std::map<std::uint32_t, std::unique_ptr<llvm::MemoryBuffer>> buffers;
    std::map<std::uint32_t, Draft> drafts;
    for(auto file: files) {
        auto& info = facts.files[file];
        llvm::SmallString<256> path(root);
        llvm::sys::path::append(path, posix, info.path);
        auto buffer = vfs::read(path);
        if(!buffer) {
            return std::unexpected(
                std::format("cannot read {}: {}", path.str().str(), buffer.error().message()));
        }
        auto& draft = drafts[file];
        Text text((*buffer)->getBuffer());
        buffers[file] = std::move(*buffer);
        auto module_unit = rewriter.rewritten(file);

        // Declarations of other modules' entities: in a module unit they
        // would declare a second entity, attached to its module.
        llvm::DenseSet<std::uint32_t> dropped;
        if(module_unit) {
            auto drop = [&](std::uint32_t line) {
                if(line != 0 && line <= text.lines.size() && text.forward_declaration(line - 1)) {
                    dropped.insert(line - 1);
                }
            };
            for(auto& redeclaration: facts.redeclarations) {
                auto owner = facts.entities[redeclaration.entity].owner;
                if(redeclaration.file == file && !redeclaration.definition &&
                   !redeclaration.friend_declaration &&
                   partition.module_of[owner] != partition.module_of[file]) {
                    drop(redeclaration.line);
                }
            }
            for(auto& foreign: facts.foreign_declarations) {
                if(foreign.file == file && !foreign.friend_declaration) {
                    drop(foreign.line);
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
            if(text.kinds[line] == Text::Line::Directive && keyword == "pragma" &&
               text.lines[line].contains("once")) {
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
                        if(target != none && rewriter.textual(target)) {
                            draft.textual.insert(target);
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
        for(auto defining: units[file].macros) {
            if(rewriter.rewritten(defining) && rewriter.header(defining)) {
                draft.macro_headers.insert(defining);
            }
        }
        draft.modules.erase(partition.modules[partition.module_of[file]]);
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

    auto include = [&](std::uint32_t file) -> std::optional<std::string> {
        auto spelled = rewriter.spelling(file);
        if(spelled.empty()) {
            plan.warnings.push_back(
                std::format("no include names {}, which other files need", facts.files[file].path));
            return std::nullopt;
        }
        return std::format("#include {}", spelled);
    };
    auto macro_header = [&](std::uint32_t file) {
        return with_extension(facts.files[file].path, "macros.h");
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
            if(auto line = include(needed)) {
                lines.push_back(std::move(*line));
            }
        }
        std::set<std::string> macro_includes;
        for(auto defining: draft.macro_headers) {
            if(defining == file) {
                continue;
            }
            macro_headers.insert(defining);
            // Named the way the header is, `"support/logging.h"` giving
            // `"support/logging.macros.h"`.
            llvm::StringRef spelled = rewriter.spelling(defining);
            auto named = spelled.empty()
                             ? macro_header(defining)
                             : with_extension(spelled.drop_front().drop_back(), "macros.h");
            auto [open, close] =
                spelled.starts_with("<") ? std::pair{'<', '>'} : std::pair{'"', '"'};
            macro_includes.insert(std::format("#include {}{}{}", open, named, close));
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

    for(auto defining: macro_headers) {
        Text text(buffers[defining]->getBuffer());
        std::vector<std::string> lines{"#pragma once", ""};
        for(std::uint32_t line = 0; line < text.lines.size(); line += 1) {
            auto keyword = text.keywords[line];
            if(text.kinds[line] != Text::Line::Directive || takes_header_name(keyword) ||
               keyword == "pragma") {
                continue;
            }
            for(auto part = line; part <= text.ends[line]; part += 1) {
                lines.push_back(text.lines[part].str());
            }
        }
        auto path = macro_header(defining);
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
