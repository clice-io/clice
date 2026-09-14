#include "semantic/content.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "semantic/decls.h"
#include "semantic/hasher.h"
#include "semantic/semantics.h"
#include "semantic/types.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/Sanitizers.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Basic/Version.h"
#include "clang/Lex/MacroInfo.h"

namespace clice {

namespace {

/// Bump when the set or order of hashed inputs changes: content values
/// only compare within one scheme.
constexpr std::uint64_t content_scheme = 1;

constexpr std::uint32_t no_unit = static_cast<std::uint32_t>(-1);

class Hasher : public ByteHasher {
public:
    using ByteHasher::add;

    void add(ContentHash hash) {
        add(hash.low);
        add(hash.high);
    }

    ContentHash finish() const {
        auto data = bytes();
        auto hash = llvm::xxh3_128bits(
            llvm::ArrayRef(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
        return {.low = hash.low64, .high = hash.high64};
    }
};

template <typename Range>
void add_list(Hasher& hasher, const Range& values) {
    hasher.add(static_cast<std::uint64_t>(std::ranges::distance(values)));
    for(auto& value: values) {
        hasher.add(llvm::StringRef(value));
    }
}

/// Everything about the compile that changes diagnostics without changing
/// any file: the language mode, the target and the warning flags. One
/// value per TU, folded into every unit's own so equal content still
/// implies equal diagnostics by a single comparison.
ContentHash compile_context(CompilationUnitRef unit) {
    Hasher hasher;
    hasher.add(content_scheme);
    hasher.add(clang::getClangFullVersion());

    auto& lang = unit.lang_options();
#define LANGOPT(Name, Bits, Default, Compatibility, Description)                                   \
    hasher.add(static_cast<std::uint64_t>(lang.Name));
#define ENUM_LANGOPT(Name, Type, Bits, Default, Compatibility, Description)                        \
    hasher.add(static_cast<std::uint64_t>(lang.get##Name()));
#include "clang/Basic/LangOptions.def"
    hasher.add(static_cast<std::uint64_t>(lang.LangStd));
    add_list(hasher, lang.NoBuiltinFuncs);
    add_list(hasher, lang.ModuleFeatures);
    add_list(hasher, lang.CommentOpts.BlockCommandNames);
    hasher.add(static_cast<std::uint64_t>(lang.CommentOpts.ParseAllComments));
    hasher.add(lang.CXXABI ? static_cast<std::uint64_t>(*lang.CXXABI) + 1 : 0);
    hasher.add(static_cast<std::uint64_t>(lang.OverflowPatternExclusionMask));
    hasher.add(lang.RandstructSeed);
    hasher.add(static_cast<std::uint64_t>(lang.CheckNew));
    hasher.add(static_cast<std::uint64_t>(lang.IsHeaderFile));
#define SANITIZER(NAME, ID)                                                                        \
    hasher.add(static_cast<std::uint64_t>(lang.Sanitize.has(clang::SanitizerKind::ID)));
#include "clang/Basic/Sanitizers.def"

    auto& target = unit.context().getTargetInfo();
    auto& options = target.getTargetOpts();
    hasher.add(options.Triple);
    hasher.add(options.CPU);
    hasher.add(options.TuneCPU);
    hasher.add(options.FPMath);
    hasher.add(options.ABI);
    hasher.add(static_cast<std::uint64_t>(options.EABIVersion));
    add_list(hasher, options.Features);
    hasher.add(options.CodeModel);
    hasher.add(target.getDataLayoutString());

    auto& engine = unit.context().getDiagnostics();
    auto& diagnostics = engine.getDiagnosticOptions();
    add_list(hasher, diagnostics.Warnings);
    add_list(hasher, diagnostics.Remarks);
#define DIAGOPT(Name, Bits, Default) hasher.add(static_cast<std::uint64_t>(diagnostics.Name));
#define ENUM_DIAGOPT(Name, Type, Bits, Default)                                                    \
    hasher.add(static_cast<std::uint64_t>(diagnostics.get##Name()));
#include "clang/Basic/DiagnosticOptions.def"
    // The state the flags produced: `-Wsystem-headers`, `-w`, `-Weverything`
    // and `-Werror` live on the engine, not in the options.
    hasher.add(static_cast<std::uint64_t>(engine.getSuppressSystemWarnings()));
    hasher.add(static_cast<std::uint64_t>(engine.getIgnoreAllWarnings()));
    hasher.add(static_cast<std::uint64_t>(engine.getEnableAllWarnings()));
    hasher.add(static_cast<std::uint64_t>(engine.getWarningsAsErrors()));
    hasher.add(static_cast<std::uint64_t>(engine.getErrorsAsFatal()));
    hasher.add(static_cast<std::uint64_t>(engine.getExtensionHandlingBehavior()));
    return hasher.finish();
}

/// A NOLINTBEGIN or NOLINTEND marker of a file with the check list written
/// after it. Found by a raw text scan, like clang-tidy's own
/// NoLintDirectiveHandler: a marker in a string literal counts there too.
struct Suppression {
    std::uint32_t offset;
    llvm::StringRef text;
};

constexpr llvm::StringRef nolint = "NOLINT";
constexpr llvm::StringRef nolint_begin = "NOLINTBEGIN";
constexpr llvm::StringRef nolint_end = "NOLINTEND";

std::vector<Suppression> scan_suppressions(llvm::StringRef content) {
    std::vector<Suppression> out;
    for(auto pos = content.find(nolint); pos != llvm::StringRef::npos;
        pos = content.find(nolint, pos + nolint.size())) {
        auto rest = content.substr(pos);
        std::size_t length = 0;
        if(rest.starts_with(nolint_begin)) {
            length = nolint_begin.size();
        } else if(rest.starts_with(nolint_end)) {
            length = nolint_end.size();
        } else {
            continue;
        }
        if(rest.size() > length && rest[length] == '(') {
            auto close = rest.find(')', length);
            length = close == llvm::StringRef::npos ? rest.size() : close + 1;
        }
        out.push_back({.offset = static_cast<std::uint32_t>(pos), .text = rest.take_front(length)});
    }
    return out;
}

template <typename Container>
void sort_unique(Container& values) {
    std::ranges::sort(values);
    auto duplicates = std::ranges::unique(values);
    values.erase(duplicates.begin(), duplicates.end());
}

std::uint32_t line_begin(llvm::StringRef content, std::uint32_t offset) {
    auto newline = content.rfind('\n', offset);
    return newline == llvm::StringRef::npos ? 0 : static_cast<std::uint32_t>(newline + 1);
}

/// The start of the line before the one containing `offset`: where a
/// `// NOLINTNEXTLINE` for it would sit.
std::uint32_t previous_line_begin(llvm::StringRef content, std::uint32_t offset) {
    auto line = line_begin(content, offset);
    return line > 0 ? line_begin(content, line - 1) : 0;
}

/// One past the newline ending the line containing `offset`, or the file end.
std::uint32_t line_end(llvm::StringRef content, std::uint32_t offset) {
    auto newline = content.find('\n', offset);
    return newline == llvm::StringRef::npos ? static_cast<std::uint32_t>(content.size())
                                            : static_cast<std::uint32_t>(newline + 1);
}

/// A unit's spelled span before merging and partitioning: where its
/// written declarations sit in the file.
struct Candidate {
    std::uint32_t node;
    clang::FileID fid;
    std::uint32_t begin;
    std::uint32_t end;
};

struct SccNode {
    std::uint32_t unit;
    llvm::SmallVector<SccNode*, 4> children;
};

}  // namespace

}  // namespace clice

template <>
struct llvm::GraphTraits<clice::SccNode*> {
    using NodeRef = clice::SccNode*;
    using ChildIteratorType = clice::SccNode**;

    static NodeRef getEntryNode(clice::SccNode* root) {
        return root;
    }

    static ChildIteratorType child_begin(NodeRef node) {
        return node->children.begin();
    }

    static ChildIteratorType child_end(NodeRef node) {
        return node->children.end();
    }
};

namespace clice {

namespace {

class ContentBuilder {
public:
    ContentBuilder(CompilationUnitRef unit, const Semantics& semantics, ContentTable& table) :
        unit(unit), SM(unit.context().getSourceManager()), semantics(semantics), table(table) {}

    void build() {
        enumerate();
        map_fragments();
        attribute_nodes();
        collect_deps();
        hash_own();
        close();
        hash_elements();
        digest();
    }

private:
    using Node = Semantics::Node;

    /// Only the AST segment of the table forms units; the preprocessor
    /// nodes after it are consumed through the directives instead.
    std::uint32_t ast_node_count() const {
        auto entries = semantics.node_entries();
        auto it =
            std::ranges::find_if_not(entries, [](const Node& node) { return node.node.is_ast(); });
        return static_cast<std::uint32_t>(it - entries.begin());
    }

    static bool is_transparent(const clang::Decl* decl) {
        return llvm::isa<clang::NamespaceDecl, clang::LinkageSpecDecl, clang::ExportDecl>(decl);
    }

    /// Walk the AST roots, descending through transparent containers, and
    /// turn every other written declaration into a candidate; then merge
    /// overlapping candidates, partition each file and fill the tables
    /// that map nodes and decls back to their unit.
    void enumerate() {
        auto entries = semantics.node_entries();
        auto count = ast_node_count();
        for(std::uint32_t i = 0; i < count; i = entries[i].subtree_end) {
            enumerate(i);
        }

        std::vector<std::uint32_t> order(candidates.size());
        std::ranges::iota(order, 0u);
        std::ranges::sort(order, {}, [&](std::uint32_t index) {
            return std::tuple(candidates[index].fid, candidates[index].begin, index);
        });

        // Candidates overlapping in one file (`int a, b;`, `struct S {} s;`)
        // become one unit holding every node.
        std::vector<std::uint32_t> unit_of_candidate(candidates.size(), no_unit);
        for(auto index: order) {
            auto& candidate = candidates[index];
            if(!unit_spans.empty() && unit_spans.back().fid == candidate.fid &&
               candidate.begin < unit_spans.back().end) {
                unit_spans.back().end = std::max(unit_spans.back().end, candidate.end);
                table.units.back().nodes.push_back(candidate.node);
            } else {
                unit_spans.push_back(candidate);
                table.units.push_back(
                    {.nodes = {candidate.node},
                     .decl = semantics.node(candidate.node).node.get<clang::Decl>(),
                     .fid = candidate.fid});
            }
            unit_of_candidate[index] = static_cast<std::uint32_t>(table.units.size() - 1);
        }

        partition();

        // Units come in (file, offset) order, so a container unit whose body
        // is an `#include` is written before the included file's units and
        // the innermost owner wins.
        node_units.assign(count, no_unit);
        for(std::uint32_t u = 0; u < table.units.size(); u += 1) {
            for(auto root: table.units[u].nodes) {
                for(std::uint32_t i = root; i < entries[root].subtree_end; i += 1) {
                    node_units[i] = u;
                }
            }
        }
        for(std::uint32_t i = 0; i < count; i += 1) {
            auto* decl = entries[i].node.get<clang::Decl>();
            if(decl && node_units[i] != no_unit) {
                decl_units[decl] = node_units[i];
            }
        }
        for(auto& [decl, candidate]: container_candidates) {
            decl_units.try_emplace(decl, unit_of_candidate[candidate]);
        }
    }

    void enumerate(std::uint32_t i) {
        const Node& node = semantics.node(i);
        auto* decl = node.node.get<clang::Decl>();
        if(!decl) {
            return;
        }

        if(is_transparent(decl)) {
            auto before = candidates.size();
            for(std::uint32_t j = i + 1; j < node.subtree_end; j = semantics.node(j).subtree_end) {
                enumerate(j);
            }
            // A container's header text lands in its first unit of the same
            // file (trivia belongs to the following unit), so references to
            // the container resolve there. A container with nothing written
            // in its own file — empty, or wrapping only an `#include` — is a
            // unit of its own, so a diagnostic on its name has an owner.
            auto span = spelled_span(i);
            if(!span) {
                return;
            }
            auto own_file = std::ranges::find(candidates.begin() + before,
                                              candidates.end(),
                                              span->fid,
                                              &Candidate::fid);
            auto index = static_cast<std::uint32_t>(own_file - candidates.begin());
            if(own_file == candidates.end()) {
                candidates.push_back(*span);
            }
            container_candidates.emplace_back(decl, index);
            return;
        }

        if(node.flags.in_instantiation) {
            return;
        }
        // The function and variable forms of an explicit instantiation
        // directive sit at their pattern's location in clang 22 (see
        // decls::is_instantiation); under the instantiation option they
        // reach the table inside their template anyway.
        if(decls::is_instantiation(decl) &&
           !llvm::isa<clang::ClassTemplateSpecializationDecl>(decl)) {
            return;
        }

        if(auto span = spelled_span(i)) {
            candidates.push_back(*span);
        }
    }

    /// The file range the written declarations of subtree `i` cover:
    /// every unflagged Decl node's expansion range, a body that ends in an
    /// included file (a fragment) walked back up to the `#include` line.
    std::optional<Candidate> spelled_span(std::uint32_t i) {
        std::optional<Candidate> span;
        auto end = semantics.node(i).subtree_end;
        for(std::uint32_t j = i; j < end; j += 1) {
            const Node& node = semantics.node(j);
            auto* decl = node.node.get<clang::Decl>();
            if(!decl) {
                continue;
            }
            // An instantiation subtree reuses the pattern's locations: an
            // explicit function or variable instantiation under its
            // template is unflagged but sits at the pattern's definition
            // (decls::is_instantiation), possibly in another declaration.
            if(node.flags.in_instantiation || (j != i && decls::is_instantiation(decl))) {
                j = node.subtree_end - 1;
                continue;
            }
            auto range = SM.getExpansionRange(decl->getSourceRange());
            if(range.isInvalid()) {
                continue;
            }
            auto [fid, begin] = SM.getDecomposedLoc(range.getBegin());
            if(span && span->fid != fid) {
                continue;
            }
            if(!span) {
                if(unit.is_builtin_file(fid)) {
                    return std::nullopt;
                }
                span = Candidate{.node = i, .fid = fid, .begin = begin, .end = begin};
            }

            auto last = range.getEnd();
            while(SM.getFileID(last) != fid) {
                auto include = SM.getIncludeLoc(SM.getFileID(last));
                if(include.isInvalid()) {
                    last = clang::SourceLocation();
                    break;
                }
                last = include;
            }
            std::uint32_t stop = last.isValid()
                                     ? SM.getFileOffset(last) + unit.token_length(last)
                                     : static_cast<std::uint32_t>(unit.file_content(fid).size());
            span->begin = std::min(span->begin, begin);
            span->end = std::max(span->end, stop);
        }
        return span;
    }

    /// Cut every file into consecutive unit ranges: a unit ends after the
    /// `;` following its span and the rest of that line, but never past the
    /// next span's start; the file's leading text belongs to its first
    /// unit, the trailing text to its last.
    void partition() {
        for(std::uint32_t u = 0; u < unit_spans.size(); u += 1) {
            auto& span = unit_spans[u];
            auto& current = table.units[u];
            bool first = u == 0 || unit_spans[u - 1].fid != span.fid;
            bool last = u + 1 == unit_spans.size() || unit_spans[u + 1].fid != span.fid;
            llvm::StringRef content = unit.file_content(span.fid);

            std::uint32_t end = span.end;
            while(end < content.size() && (content[end] == ' ' || content[end] == '\t')) {
                end += 1;
            }
            if(end < content.size() && content[end] == ';') {
                end += 1;
            }
            end = line_end(content, end);
            if(last) {
                end = static_cast<std::uint32_t>(content.size());
            } else {
                end = std::min(end, unit_spans[u + 1].begin);
            }

            current.range = {first ? 0 : table.units[u - 1].range.end, end};
            if(first) {
                file_units[span.fid].first = u;
            }
            file_units[span.fid].last = u;
        }
    }

    /// A `#include` entered from inside a unit's span makes the included
    /// file — and whatever it includes — a fragment of that unit, unless
    /// the file has units of its own (`extern "C" { #include <c.h> }`
    /// wraps top-level declarations, it does not swallow them). An
    /// include the preprocessor could not enter has no file.
    void map_fragments() {
        llvm::SmallVector<std::pair<clang::FileID, std::uint32_t>> pending;
        for(auto& [fid, directive]: unit.directives()) {
            for(auto& include: directive.includes) {
                if(include.skipped || include.fid.isInvalid() || include.location.isInvalid() ||
                   file_units.contains(include.fid)) {
                    continue;
                }
                auto [at, offset] = SM.getDecomposedLoc(SM.getExpansionLoc(include.location));
                auto owner = unit_at(at, offset);
                if(owner != no_unit && unit_spans[owner].begin <= offset &&
                   offset < unit_spans[owner].end) {
                    pending.emplace_back(include.fid, owner);
                }
            }
        }

        while(!pending.empty()) {
            auto [fid, owner] = pending.pop_back_val();
            if(!fragment_owner.try_emplace(fid, owner).second) {
                continue;
            }
            table.units[owner].fragments.push_back(fid);
            if(auto it = unit.directives().find(fid); it != unit.directives().end()) {
                for(auto& include: it->second.includes) {
                    if(!include.skipped && include.fid.isValid() &&
                       !file_units.contains(include.fid)) {
                        pending.emplace_back(include.fid, owner);
                    }
                }
            }
        }
    }

    /// The unit whose range holds `offset` of `fid`.
    std::uint32_t unit_at(clang::FileID fid, std::uint32_t offset) const {
        auto it = file_units.find(fid);
        if(it == file_units.end()) {
            return no_unit;
        }
        auto [first, last] = it->second;
        auto range = std::views::iota(first, last + 1);
        auto found = std::ranges::partition_point(range, [&](std::uint32_t u) {
            return table.units[u].range.end <= offset;
        });
        return found == range.end() ? no_unit : *found;
    }

    /// The unit a token at a file location belongs to: through the
    /// fragment table for included bodies.
    std::uint32_t token_owner(clang::FileID fid, std::uint32_t offset) const {
        if(auto it = fragment_owner.find(fid); it != fragment_owner.end()) {
            return it->second;
        }
        return unit_at(fid, offset);
    }

    /// The unit a node's references count for: the unit whose subtree
    /// holds it, except inside an instantiation subtree, which counts for
    /// no unit's content — it is written nowhere, and letting it in would
    /// make the pattern's content vary with every TU's instantiations.
    /// Instead each materialized instantiation becomes an element of its
    /// pattern's unit (the unit holding the lambda for a generic lambda's
    /// call operator, which has no pattern node); nodes inside it are
    /// attributed to the innermost such element. An instantiation the
    /// compiler only declared (`decltype(f<double>)`) is nothing at all.
    void attribute_nodes() {
        auto entries = semantics.node_entries();
        attribution = node_units;
        element_of_node.assign(attribution.size(), no_unit);
        llvm::SmallVector<std::pair<std::uint32_t, std::uint32_t>, 8> stack;
        for(std::uint32_t i = 0; i < attribution.size(); i += 1) {
            while(!stack.empty() && stack.back().first <= i) {
                stack.pop_back();
            }
            auto* decl = entries[i].node.get<clang::Decl>();
            if(decl && llvm::isa<clang::FunctionDecl, clang::CXXRecordDecl, clang::VarDecl>(decl) &&
               decls::is_instantiation(decl)) {
                auto element = no_unit;
                if(auto mask = materialized(decl)) {
                    auto owner = lookup(decls::normalize(llvm::cast<clang::NamedDecl>(decl)));
                    if(owner == no_unit) {
                        owner = node_units[i];
                    }
                    if(owner != no_unit) {
                        element = static_cast<std::uint32_t>(heads.size());
                        heads.push_back({.owner = owner,
                                         .entity = unit.entity(llvm::cast<clang::NamedDecl>(decl)),
                                         .mask = *mask});
                        argument_targets(decl, heads.back().targets);
                    }
                }
                stack.emplace_back(entries[i].subtree_end, element);
            }
            if(!stack.empty()) {
                attribution[i] = no_unit;
                element_of_node[i] = stack.back().second;
            }
        }
    }

    /// The unit of a decl: its own node, or — for a declaration the
    /// traversal never records, like an implicit member — the nearest
    /// lexically enclosing declaration that has one.
    std::uint32_t lookup(const clang::Decl* decl) const {
        if(auto it = decl_units.find(decl); it != decl_units.end()) {
            return it->second;
        }
        for(auto* context = decl->getLexicalDeclContext(); context;
            context = context->getLexicalParent()) {
            if(auto it = decl_units.find(llvm::cast<clang::Decl>(context));
               it != decl_units.end()) {
                return it->second;
            }
        }
        return no_unit;
    }

    void add_target(const clang::Decl* decl, llvm::SmallVectorImpl<std::uint32_t>& out) const {
        if(!decl) {
            return;
        }
        for(auto* redecl: decl->redecls()) {
            if(auto u = lookup(redecl); u != no_unit) {
                out.push_back(u);
            }
        }
    }

    /// The units a referenced declaration lives in: every redeclaration,
    /// and for a template or a specialization the whole family — the
    /// template's redeclarations and its explicit and partial
    /// specializations, since which of them a use selects is part of what
    /// the use's diagnostics depend on.
    void targets(const clang::NamedDecl* decl, llvm::SmallVectorImpl<std::uint32_t>& out) const {
        auto* canonical = decls::normalize(decl);
        add_target(canonical, out);

        const clang::TemplateDecl* family = llvm::dyn_cast<clang::TemplateDecl>(canonical);
        if(!family) {
            family = canonical->getDescribedTemplate();
        }
        if(auto* CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(canonical)) {
            family = CTSD->getSpecializedTemplate();
        } else if(auto* FD = llvm::dyn_cast<clang::FunctionDecl>(canonical); FD && !family) {
            family = FD->getPrimaryTemplate();
        } else if(auto* VTSD = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(canonical)) {
            family = VTSD->getSpecializedTemplate();
        }
        if(!family) {
            return;
        }

        add_target(family, out);
        if(auto* CTD = llvm::dyn_cast<clang::ClassTemplateDecl>(family)) {
            for(auto* spec: CTD->specializations()) {
                if(spec->getSpecializationKind() == clang::TSK_ExplicitSpecialization) {
                    add_target(spec, out);
                }
            }
            llvm::SmallVector<clang::ClassTemplatePartialSpecializationDecl*, 4> partials;
            CTD->getPartialSpecializations(partials);
            for(auto* partial: partials) {
                add_target(partial, out);
            }
        } else if(auto* FTD = llvm::dyn_cast<clang::FunctionTemplateDecl>(family)) {
            for(auto* spec: FTD->specializations()) {
                if(spec->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
                    add_target(spec, out);
                }
            }
        } else if(auto* VTD = llvm::dyn_cast<clang::VarTemplateDecl>(family)) {
            for(auto* spec: VTD->specializations()) {
                if(spec->getSpecializationKind() == clang::TSK_ExplicitSpecialization) {
                    add_target(spec, out);
                }
            }
            llvm::SmallVector<clang::VarTemplatePartialSpecializationDecl*, 4> partials;
            VTD->getPartialSpecializations(partials);
            for(auto* partial: partials) {
                add_target(partial, out);
            }
        }
    }

    /// Edges from every reference, and per unit the entities the
    /// references selected: a dependency set alone cannot tell which of
    /// two overloads a call bound to, but the diagnostics can.
    /// The declarations a specialization's template arguments name: an
    /// instantiated body depends on their definitions even where it never
    /// spells the parameter (`sizeof(T)` written as `__builtin_choose_expr`
    /// on it), and equal entities do not mean equal definitions.
    void argument_targets(const clang::Decl* decl,
                          llvm::SmallVectorImpl<std::uint32_t>& out) const {
        const clang::TemplateArgumentList* arguments = nullptr;
        if(auto* CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
            arguments = &CTSD->getTemplateArgs();
        } else if(auto* FD = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            arguments = FD->getTemplateSpecializationArgs();
        } else if(auto* VTSD = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(decl)) {
            arguments = &VTSD->getTemplateArgs();
        }
        if(arguments) {
            for(auto& argument: arguments->asArray()) {
                add_argument(argument, out);
            }
        }
    }

    void add_argument(const clang::TemplateArgument& argument,
                      llvm::SmallVectorImpl<std::uint32_t>& out) const {
        switch(argument.getKind()) {
            case clang::TemplateArgument::Type: {
                auto type = argument.getAsType().getNonReferenceType();
                while(true) {
                    if(auto* pointer = type->getAs<clang::PointerType>()) {
                        type = pointer->getPointeeType();
                    } else if(auto* array = type->getAsArrayTypeUnsafe()) {
                        type = array->getElementType();
                    } else {
                        break;
                    }
                }
                for(auto* decl: types::decls_of(type)) {
                    targets(decl, out);
                }
                break;
            }
            case clang::TemplateArgument::Declaration: {
                targets(argument.getAsDecl(), out);
                break;
            }
            case clang::TemplateArgument::Template:
            case clang::TemplateArgument::TemplateExpansion: {
                if(auto* TD = argument.getAsTemplateOrTemplatePattern().getAsTemplateDecl()) {
                    targets(TD, out);
                }
                break;
            }
            case clang::TemplateArgument::Pack: {
                for(auto& element: argument.pack_elements()) {
                    add_argument(element, out);
                }
                break;
            }
            case clang::TemplateArgument::Null:
            case clang::TemplateArgument::NullPtr:
            case clang::TemplateArgument::Integral:
            case clang::TemplateArgument::StructuralValue:
            case clang::TemplateArgument::Expression: {
                break;
            }
        }
    }

    /// Whether a declaration's entity carries its source position: locals
    /// and parameters, template parameters and lambda members are told
    /// apart by where they are written (semantic/identity.h).
    static bool positional(const clang::NamedDecl* decl) {
        if(decl->getParentFunctionOrMethod() || llvm::isa<clang::ParmVarDecl>(decl) ||
           decl->isTemplateParameter()) {
            return true;
        }
        auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl->getDeclContext());
        return record && record->isLambda();
    }

    void collect_deps() {
        auto& resolver = unit.resolver();
        std::vector<llvm::DenseSet<std::uint32_t>> deps(table.units.size());
        referenced.resize(table.units.size());
        llvm::SmallVector<std::uint32_t, 8> found;
        for(std::uint32_t i = 0; i < attribution.size(); i += 1) {
            auto owner = attribution[i];
            auto element = element_of_node[i];
            if(owner == no_unit && element == no_unit) {
                continue;
            }
            for(auto& reference: resolve_references(semantics, i, &resolver)) {
                // The unit's own text already tells which local a name binds
                // to, so neither a positional entity nor a declaration of the
                // unit's own goes into the selected set.
                bool selected = !reference.kind.isDeclOrDef() && !positional(reference.decl);
                found.clear();
                if(reference.kind.isDeclOrDef() && lookup(reference.decl) == owner) {
                    add_target(reference.decl, found);
                } else {
                    targets(reference.decl, found);
                }
                if(element != no_unit) {
                    auto& head = heads[element];
                    if(selected) {
                        head.entities.push_back(unit.entity(decls::normalize(reference.decl)));
                    }
                    head.targets.append(found);
                    continue;
                }
                if(selected) {
                    referenced[owner].push_back(unit.entity(decls::normalize(reference.decl)));
                }
                for(auto target: found) {
                    if(target != owner) {
                        deps[owner].insert(target);
                    }
                }
            }
        }
        for(std::uint32_t u = 0; u < deps.size(); u += 1) {
            table.units[u].deps.assign(deps[u].begin(), deps[u].end());
            std::ranges::sort(table.units[u].deps);
        }
    }

    /// The outcome of every conditional directive of a file, by offset:
    /// which branches the preprocessor took decides what a `#warning` or
    /// `#error` inside the unit's text did, and no token records it.
    struct Branch {
        std::uint32_t offset;
        Condition::BranchKind kind;
        Condition::ConditionValue value;
    };

    const std::vector<Branch>& branches(clang::FileID fid) {
        auto [it, inserted] = branch_cache.try_emplace(fid);
        if(inserted) {
            if(auto found = unit.directives().find(fid); found != unit.directives().end()) {
                for(auto& condition: found->second.conditions) {
                    if(condition.loc.isValid()) {
                        it->second.push_back({.offset = SM.getFileOffset(condition.loc),
                                              .kind = condition.kind,
                                              .value = condition.value});
                    }
                }
                std::ranges::sort(it->second, {}, &Branch::offset);
            }
        }
        return it->second;
    }

    /// What every `#include` inside the unit's text resolved to: whether
    /// the preprocessor entered the file and the characteristic it got
    /// (`-I` versus `-isystem` for the same spelling decides what
    /// include-level checks report).
    void add_includes(Hasher& hasher, clang::FileID fid, LocalSourceRange range) {
        auto found = unit.directives().find(fid);
        if(found == unit.directives().end()) {
            hasher.add(static_cast<std::uint64_t>(0));
            return;
        }
        std::uint64_t count = 0;
        Hasher inner;
        for(auto& include: found->second.includes) {
            if(include.location.isInvalid()) {
                continue;
            }
            auto offset = SM.getFileOffset(SM.getExpansionLoc(include.location));
            if(offset < range.begin || offset >= range.end) {
                continue;
            }
            count += 1;
            inner.add(static_cast<std::uint64_t>(include.skipped));
            inner.add(static_cast<std::uint64_t>(
                include.fid.isValid()
                    ? SM.getFileCharacteristic(SM.getLocForStartOfFile(include.fid)) + 1
                    : 0));
        }
        hasher.add(count);
        hasher.add(inner.finish());
    }

    void add_branches(Hasher& hasher, clang::FileID fid, LocalSourceRange range) {
        auto& all = branches(fid);
        auto first = std::ranges::lower_bound(all, range.begin, {}, &Branch::offset);
        auto last = std::ranges::lower_bound(all, range.end, {}, &Branch::offset);
        hasher.add(static_cast<std::uint64_t>(last - first));
        for(auto it = first; it != last; it += 1) {
            hasher.add(static_cast<std::uint64_t>(it->kind));
            hasher.add(static_cast<std::uint64_t>(it->value));
        }
    }

    const std::vector<Suppression>& suppressions(clang::FileID fid) {
        auto [it, inserted] = suppression_cache.try_emplace(fid);
        if(inserted) {
            it->second = scan_suppressions(unit.file_content(fid));
        }
        return it->second;
    }

    /// What decides whether a diagnostic at `offset` of `fid` is
    /// suppressed: the line before it (NOLINTNEXTLINE) is hashed by the
    /// caller as text; here the file's NOLINTBEGIN/NOLINTEND sequence and
    /// the position among them.
    void add_suppression_context(Hasher& hasher, clang::FileID fid, std::uint32_t offset) {
        auto& markers = suppressions(fid);
        hasher.add(static_cast<std::uint64_t>(markers.size()));
        std::uint64_t before = 0;
        for(auto& marker: markers) {
            hasher.add(marker.text);
            before += marker.offset < offset;
        }
        hasher.add(before);
    }

    /// The diagnostic pragmas of the TU in the order the preprocessor ran
    /// them, and the hash of every prefix: a unit's diagnostic state is the
    /// prefix executed before it.
    void prepare_pragmas() {
        std::vector<const DiagnosticPragma*> pragmas;
        for(auto& [fid, directive]: unit.directives()) {
            for(auto& pragma: directive.diagnostic_pragmas) {
                pragmas.push_back(&pragma);
            }
        }
        // Two pragmas one macro expands to share a location; they were
        // recorded in execution order, which a stable sort keeps.
        std::ranges::stable_sort(pragmas,
                                 [&](const DiagnosticPragma* a, const DiagnosticPragma* b) {
                                     return SM.isBeforeInTranslationUnit(a->loc, b->loc);
                                 });

        pragma_locations.reserve(pragmas.size());
        pragma_prefixes.reserve(pragmas.size() + 1);
        pragma_prefixes.push_back(Hasher().finish());
        for(auto* pragma: pragmas) {
            Hasher hasher;
            hasher.add(pragma_prefixes.back());
            hasher.add(static_cast<std::uint64_t>(pragma->kind));
            hasher.add(static_cast<std::uint64_t>(pragma->severity));
            hasher.add(pragma->flag);
            pragma_prefixes.push_back(hasher.finish());
            pragma_locations.push_back(pragma->loc);
        }
    }

    ContentHash pragma_state(clang::SourceLocation location) const {
        auto it = std::ranges::partition_point(pragma_locations, [&](clang::SourceLocation loc) {
            return SM.isBeforeInTranslationUnit(loc, location);
        });
        return pragma_prefixes[it - pragma_locations.begin()];
    }

    /// The macros whose expansions produced tokens of the expansion
    /// `fid`, outermost last: every level of the caller chain, and for an
    /// argument the parameter's own chain, which names the macro the
    /// argument was substituted into.
    void expansion_macros(clang::SourceLocation location,
                          llvm::SmallVectorImpl<const clang::MacroInfo*>& out) const {
        while(location.isMacroID()) {
            auto range = SM.getImmediateExpansionRange(location);
            if(SM.isMacroArgExpansion(location)) {
                expansion_macros(range.getBegin(), out);
                location = SM.getImmediateSpellingLoc(location);
                continue;
            }
            if(auto it = expansions.find(range.getBegin().getRawEncoding());
               it != expansions.end()) {
                out.push_back(it->second);
            }
            location = range.getBegin();
        }
    }

    const llvm::SmallVector<const clang::MacroInfo*, 2>& macros_of(clang::SourceLocation location) {
        auto [it, inserted] = expansion_cache.try_emplace(SM.getFileID(location));
        if(inserted) {
            expansion_macros(location, it->second);
        }
        return it->second;
    }

    /// The definition lines of a macro and the suppression context at
    /// them: a NOLINT on the `#define` line or the line before it silences
    /// diagnostics at every expansion. Nullopt for a builtin.
    std::optional<ContentHash> macro_definition(const clang::MacroInfo* macro) {
        auto begin = macro->getDefinitionLoc();
        auto end = macro->getDefinitionEndLoc();
        if(macro->isBuiltinMacro() || begin.isInvalid() || !begin.isFileID()) {
            return std::nullopt;
        }
        auto [fid, offset] = SM.getDecomposedLoc(begin);
        llvm::StringRef content = unit.file_content(fid);
        auto from = previous_line_begin(content, offset);
        auto to = end.isValid() && SM.getFileID(end) == fid
                      ? line_end(content, SM.getFileOffset(end))
                      : line_end(content, offset);
        Hasher hasher;
        hasher.add(content.slice(from, to));
        add_suppression_context(hasher, fid, offset);
        // A macro defined in a system header has its expansions' diagnostics
        // suppressed as system macros.
        hasher.add(static_cast<std::uint64_t>(SM.getFileCharacteristic(begin)));
        return hasher.finish();
    }

    /// Every attribute of a declaration: what pragmas and the target
    /// decided about it (`#pragma pack`, `#pragma clang attribute push`,
    /// vtordisp, inheritance model) is not in its text, and the written
    /// ones cost nothing to repeat.
    void add_attributes(Hasher& hasher, const clang::Decl* decl) {
        auto& policy = unit.context().getPrintingPolicy();
        for(auto* attr: decl->attrs()) {
            hasher.add(static_cast<std::uint64_t>(attr->getKind()));
            hasher.add(llvm::StringRef(attr->getSpelling()));
            std::string pretty;
            llvm::raw_string_ostream stream(pretty);
            attr->printPretty(stream, policy);
            hasher.add(pretty);

            if(auto* MFA = llvm::dyn_cast<clang::MaxFieldAlignmentAttr>(attr)) {
                hasher.add(static_cast<std::uint64_t>(MFA->getAlignment()));
            } else if(auto* VD = llvm::dyn_cast<clang::MSVtorDispAttr>(attr)) {
                hasher.add(static_cast<std::uint64_t>(VD->getVtorDispMode()));
            } else if(auto* MI = llvm::dyn_cast<clang::MSInheritanceAttr>(attr)) {
                hasher.add(static_cast<std::uint64_t>(MI->getInheritanceModel()));
            } else if(auto* AA = llvm::dyn_cast<clang::AlignedAttr>(attr)) {
                if(!AA->isAlignmentDependent()) {
                    hasher.add(static_cast<std::uint64_t>(AA->getAlignment(unit.context())));
                }
            } else if(auto* VA = llvm::dyn_cast<clang::VisibilityAttr>(attr)) {
                hasher.add(static_cast<std::uint64_t>(VA->getVisibility()));
            } else if(auto* TVA = llvm::dyn_cast<clang::TypeVisibilityAttr>(attr)) {
                hasher.add(static_cast<std::uint64_t>(TVA->getVisibility()));
            }
        }
    }

    /// What of an instantiation the compiler materialized, as a hash of
    /// the parts, or nullopt for one it only declared: the body, and —
    /// instantiated lazily on first use — each default argument and
    /// default member initializer.
    static std::optional<std::uint64_t> materialized(const clang::Decl* decl) {
        std::string parts;
        if(auto* FD = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            parts.push_back(FD->isThisDeclarationADefinition() ? '1' : '0');
            for(auto* param: FD->parameters()) {
                parts.push_back(
                    param->hasDefaultArg() && !param->hasUninstantiatedDefaultArg() ? '1' : '0');
            }
            // A dependent exception specification is instantiated on its
            // own, when something asks for the function's type.
            auto* pattern = FD->getTemplateInstantiationPattern(false);
            auto* dependent =
                pattern ? pattern->getType()->getAs<clang::FunctionProtoType>() : nullptr;
            auto* own = FD->getType()->getAs<clang::FunctionProtoType>();
            parts.push_back(dependent && own &&
                                    dependent->getExceptionSpecType() ==
                                        clang::EST_DependentNoexcept &&
                                    own->getExceptionSpecType() != clang::EST_Uninstantiated
                                ? '1'
                                : '0');
        } else if(auto* RD = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
            if(!RD->isCompleteDefinition()) {
                return std::nullopt;
            }
            parts.push_back('1');
            for(auto* field: RD->fields()) {
                parts.push_back(field->hasInClassInitializer() &&
                                        field->getInClassInitializer() != nullptr
                                    ? '1'
                                    : '0');
            }
        } else if(auto* VD = llvm::dyn_cast<clang::VarDecl>(decl)) {
            if(VD->isThisDeclarationADefinition() != clang::VarDecl::Definition) {
                return std::nullopt;
            }
            parts.push_back('1');
        }
        if(parts.find('1') == std::string::npos) {
            return std::nullopt;
        }
        return llvm::xxh3_64bits(parts);
    }

    /// The entity a unit is known by: its first named declaration's.
    std::uint64_t entity_of(const ContentUnit& current) {
        for(auto root: current.nodes) {
            auto* decl = semantics.node(root).node.get<clang::Decl>();
            if(auto* TD = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
                decl = TD->getTemplatedDecl();
            }
            auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
            if(named &&
               (!named->getDeclName().isEmpty() || llvm::isa<clang::NamespaceDecl>(named))) {
                return unit.entity(named);
            }
        }
        return 0;
    }

    void hash_own() {
        auto entries = semantics.node_entries();
        auto context = compile_context(unit);
        prepare_pragmas();
        for(auto& [fid, directive]: unit.directives()) {
            for(auto& macro: directive.macros) {
                if(macro.kind == MacroRef::Ref) {
                    expansions.try_emplace(macro.loc.getRawEncoding(), macro.macro);
                }
            }
        }

        std::vector<Hasher> hashers(table.units.size());
        for(std::uint32_t u = 0; u < table.units.size(); u += 1) {
            auto& current = table.units[u];
            auto& span = unit_spans[u];
            auto& hasher = hashers[u];
            current.entity = entity_of(current);
            llvm::StringRef content = unit.file_content(current.fid);
            auto begin = unit.create_location(current.fid, span.begin);

            hasher.add(context);
            hasher.add(current.entity);
            hasher.add(static_cast<std::uint64_t>(SM.getFileCharacteristic(begin)));

            // The text of every line the unit touches plus, when nothing
            // of its own precedes the declaration, the line before it:
            // `// NOLINT` at the end of a shared line and
            // `// NOLINTNEXTLINE` above it.
            auto from = std::min(previous_line_begin(content, span.begin), current.range.begin);
            auto to = line_end(content, current.range.end - 1);
            hasher.add(content.slice(from, to));
            for(auto fragment: current.fragments) {
                hasher.add(unit.file_content(fragment));
            }
            for(auto fragment: current.fragments) {
                hasher.add(static_cast<std::uint64_t>(
                    SM.getFileCharacteristic(SM.getLocForStartOfFile(fragment))));
            }
            add_suppression_context(hasher, current.fid, span.begin);
            hasher.add(pragma_state(begin));
            add_branches(hasher, current.fid, current.range);
            add_includes(hasher, current.fid, current.range);
            for(auto fragment: current.fragments) {
                LocalSourceRange whole{
                    0,
                    static_cast<std::uint32_t>(unit.file_content(fragment).size())};
                add_branches(hasher, fragment, whole);
                add_includes(hasher, fragment, whole);
            }
        }

        // A transparent container's attributes (`namespace [[deprecated]] N`)
        // belong to the unit its declaration maps to; its text may sit in
        // another file when the body is an include.
        for(auto& [decl, candidate]: container_candidates) {
            if(auto it = decl_units.find(decl); it != decl_units.end()) {
                add_attributes(hashers[it->second], decl);
            }
        }

        // The macros behind macro-generated tokens are collected per unit
        // and hashed below. A diagnostic pragma executed inside a unit
        // enters its token stream where it ran: before versus after the
        // variable it silences is the difference between two diagnostic
        // sets.
        // A unit's tokens form runs in the stream; each run is hashed on
        // its own and chained into the unit's token hash, so only one run
        // is buffered at a time rather than the whole TU's token text.
        std::vector<llvm::SmallPtrSet<const clang::MacroInfo*, 4>> unit_macros(table.units.size());
        std::vector<ContentHash> token_hashes(table.units.size());
        Hasher run;
        std::uint32_t run_owner = no_unit;
        auto flush = [&] {
            if(run_owner != no_unit) {
                Hasher chain;
                chain.add(token_hashes[run_owner]);
                chain.add(run.finish());
                token_hashes[run_owner] = chain.finish();
                run = Hasher();
            }
            run_owner = no_unit;
        };
        auto append = [&](std::uint32_t owner, auto&& write) {
            if(owner != run_owner) {
                flush();
                run_owner = owner;
            }
            write(run);
        };
        std::size_t next_pragma = 0;
        auto pass_pragmas = [&](clang::SourceLocation until) {
            for(; next_pragma < pragma_locations.size() &&
                  (until.isInvalid() ||
                   SM.isBeforeInTranslationUnit(pragma_locations[next_pragma], until));
                next_pragma += 1) {
                auto [fid, offset] = SM.getDecomposedExpansionLoc(pragma_locations[next_pragma]);
                if(auto owner = token_owner(fid, offset); owner != no_unit) {
                    append(owner,
                           [&](Hasher& hasher) { hasher.add(pragma_prefixes[next_pragma + 1]); });
                }
            }
        };
        for(auto& token: unit.expanded_tokens()) {
            auto location = token.location();
            pass_pragmas(location);
            auto [fid, offset] = SM.getDecomposedLoc(SM.getExpansionLoc(location));
            auto owner = token_owner(fid, offset);
            if(owner == no_unit) {
                continue;
            }
            append(owner, [&](Hasher& hasher) {
                hasher.add(static_cast<std::uint64_t>(token.kind()));
                hasher.add(token.text(SM));
            });
            if(location.isMacroID()) {
                for(auto* macro: macros_of(location)) {
                    unit_macros[owner].insert(macro);
                }
            }
        }
        pass_pragmas(clang::SourceLocation());
        flush();

        for(std::uint32_t u = 0; u < table.units.size(); u += 1) {
            auto& current = table.units[u];
            auto& hasher = hashers[u];
            hasher.add(token_hashes[u]);

            std::vector<ContentHash> definitions;
            for(auto* macro: unit_macros[u]) {
                if(auto definition = macro_definition(macro)) {
                    definitions.push_back(*definition);
                }
            }
            sort_unique(definitions);
            hasher.add(static_cast<std::uint64_t>(definitions.size()));
            for(auto& definition: definitions) {
                hasher.add(definition);
            }

            for(auto root: current.nodes) {
                for(std::uint32_t i = root; i < entries[root].subtree_end; i += 1) {
                    if(entries[i].flags.in_instantiation || node_units[i] != u) {
                        continue;
                    }
                    if(auto* decl = entries[i].node.get<clang::Decl>()) {
                        add_attributes(hasher, decl);
                    }
                }
            }

            auto& entities = referenced[u];
            std::ranges::sort(entities);
            hasher.add(static_cast<std::uint64_t>(entities.size()));
            for(auto entity: entities) {
                hasher.add(entity);
            }

            current.own = hasher.finish();
        }
    }

    /// Strongly connected components in reverse topological order. Each
    /// component hashes its members' owns and the content of its
    /// dependencies outside it, all sorted by value; a member's content
    /// is its own combined with that. A unit outside any cycle is a
    /// component of one.
    void close() {
        auto& units = table.units;
        std::vector<SccNode> nodes(units.size() + 1);
        nodes.back().unit = static_cast<std::uint32_t>(units.size());
        for(std::uint32_t u = 0; u < units.size(); u += 1) {
            nodes[u].unit = u;
            for(auto dep: units[u].deps) {
                nodes[u].children.push_back(&nodes[dep]);
            }
            nodes.back().children.push_back(&nodes[u]);
        }

        std::vector<std::uint32_t> component(units.size(), no_unit);
        std::uint32_t next_component = 0;
        for(auto it = llvm::scc_begin(&nodes.back()); !it.isAtEnd(); ++it) {
            const std::vector<SccNode*>& scc = *it;
            if(scc.front()->unit == units.size()) {
                continue;
            }
            auto id = next_component;
            next_component += 1;
            for(auto* node: scc) {
                component[node->unit] = id;
            }

            llvm::SmallVector<ContentHash, 8> owns;
            llvm::SmallVector<ContentHash, 8> contents;
            for(auto* node: scc) {
                owns.push_back(units[node->unit].own);
                for(auto dep: units[node->unit].deps) {
                    if(component[dep] != id) {
                        contents.push_back(units[dep].content);
                    }
                }
            }
            std::ranges::sort(owns);
            sort_unique(contents);
            Hasher shared;
            for(auto& own: owns) {
                shared.add(own);
            }
            shared.add(static_cast<std::uint64_t>(contents.size()));
            for(auto& content: contents) {
                shared.add(content);
            }
            auto component_hash = shared.finish();
            for(auto* node: scc) {
                Hasher hasher;
                hasher.add(units[node->unit].own);
                hasher.add(component_hash);
                units[node->unit].content = hasher.finish();
            }
        }
    }

    /// Each materialized instantiation as one hash on its pattern's unit,
    /// computed once every content is known: what the body refers to
    /// enters as those units' content, one hop, never transitively.
    void hash_elements() {
        for(auto& head: heads) {
            Hasher hasher;
            hasher.add(head.entity);
            hasher.add(head.mask);
            llvm::SmallVector<ContentHash, 8> contents;
            sort_unique(head.targets);
            for(auto target: head.targets) {
                if(target != head.owner) {
                    contents.push_back(table.units[target].content);
                }
            }
            sort_unique(contents);
            hasher.add(static_cast<std::uint64_t>(contents.size()));
            for(auto& content: contents) {
                hasher.add(content);
            }
            std::ranges::sort(head.entities);
            hasher.add(static_cast<std::uint64_t>(head.entities.size()));
            for(auto entity: head.entities) {
                hasher.add(entity);
            }
            table.units[head.owner].elements.push_back(hasher.finish());
        }
        for(auto& current: table.units) {
            sort_unique(current.elements);
        }
    }

    void digest() {
        for(auto& [fid, range]: file_units) {
            Hasher hasher;
            hasher.add(llvm::xxh3_64bits(unit.file_content(fid)));
            for(std::uint32_t u = range.first; u <= range.last; u += 1) {
                hasher.add(table.units[u].content);
            }
            table.digests[fid] = hasher.finish();
        }
    }

    struct UnitRange {
        std::uint32_t first;
        std::uint32_t last;
    };

    CompilationUnitRef unit;
    clang::SourceManager& SM;
    const Semantics& semantics;
    ContentTable& table;

    std::vector<Candidate> candidates;
    std::vector<std::pair<const clang::Decl*, std::uint32_t>> container_candidates;

    /// Per unit, parallel to table.units: the spelled span.
    std::vector<Candidate> unit_spans;
    llvm::DenseMap<clang::FileID, UnitRange> file_units;
    llvm::DenseMap<clang::FileID, std::uint32_t> fragment_owner;

    /// Per AST node: the unit whose subtree holds it, the unit its
    /// references count for, and the element (index into heads) they count
    /// for instead inside an instantiation.
    std::vector<std::uint32_t> node_units;
    std::vector<std::uint32_t> attribution;
    std::vector<std::uint32_t> element_of_node;
    llvm::DenseMap<const clang::Decl*, std::uint32_t> decl_units;

    /// A materialized instantiation on its way to becoming an element.
    struct Head {
        std::uint32_t owner;
        std::uint64_t entity;
        std::uint64_t mask;
        llvm::SmallVector<std::uint32_t, 8> targets;
        std::vector<std::uint64_t> entities;
    };

    std::vector<Head> heads;

    /// Per unit: the entity every reference of its nodes selected.
    std::vector<std::vector<std::uint64_t>> referenced;

    llvm::DenseMap<clang::FileID, std::vector<Suppression>> suppression_cache;
    llvm::DenseMap<clang::FileID, std::vector<Branch>> branch_cache;
    std::vector<clang::SourceLocation> pragma_locations;
    std::vector<ContentHash> pragma_prefixes;

    /// Macro name location at an expansion (raw encoding) → the macro.
    llvm::DenseMap<unsigned, const clang::MacroInfo*> expansions;
    llvm::DenseMap<clang::FileID, llvm::SmallVector<const clang::MacroInfo*, 2>> expansion_cache;
};

}  // namespace

ContentTable ContentTable::compute(CompilationUnitRef unit) {
    ContentTable table;
    auto semantics = Semantics::build(unit, {.main_file_only = false, .instantiations = true});
    ContentBuilder(unit, semantics, table).build();
    return table;
}

const ContentUnit* ContentTable::find(clang::FileID fid, std::uint32_t offset) const {
    auto it = std::ranges::partition_point(units, [&](const ContentUnit& current) {
        return current.fid < fid || (current.fid == fid && current.range.end <= offset);
    });
    if(it == units.end() || it->fid != fid || it->range.begin > offset) {
        return nullptr;
    }
    return &*it;
}

}  // namespace clice
