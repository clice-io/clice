module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :feature.feature;
import :semantic.decls;
import :semantic.semantics;
import :support.text;
import :syntax.lexical_scan;
import :syntax.token;

namespace clice::feature {

namespace {

/// Of two folds over the same range, or crossing each other, the one of
/// the higher priority stays: a block the AST delimits, then one the text
/// delimits, then a section of the code around it, then a preprocessor
/// block.
enum class Priority : std::uint8_t {
    Directive,
    Section,
    Text,
    Syntax,
};

struct Candidate {
    FoldingRange fold;
    Priority priority;
};

/// The folds by position, one per range and none crossing another, the
/// ones a client can nest.
std::vector<FoldingRange> nest(std::vector<Candidate> candidates) {
    std::ranges::stable_sort(candidates, [](const Candidate& lhs, const Candidate& rhs) {
        return std::tuple(lhs.fold.range.begin, rhs.fold.range.end, rhs.priority) <
               std::tuple(rhs.fold.range.begin, lhs.fold.range.end, lhs.priority);
    });

    std::vector<bool> dropped(candidates.size());
    /// The kept folds holding the current begin, outermost first.
    llvm::SmallVector<std::size_t> open;
    for(std::size_t i = 0; i < candidates.size(); i += 1) {
        auto range = candidates[i].fold.range;
        if(i > 0 && candidates[i - 1].fold.range == range) {
            dropped[i] = true;
            continue;
        }
        while(!open.empty() && candidates[open.back()].fold.range.end <= range.begin) {
            open.pop_back();
        }
        while(!open.empty() && candidates[open.back()].fold.range.end < range.end) {
            if(candidates[open.back()].priority >= candidates[i].priority) {
                dropped[i] = true;
                break;
            }
            dropped[open.back()] = true;
            open.pop_back();
        }
        if(!dropped[i]) {
            open.push_back(i);
        }
    }

    std::vector<FoldingRange> folds;
    for(auto [candidate, gone]: llvm::zip(candidates, dropped)) {
        if(!gone) {
            folds.push_back(std::move(candidate.fold));
        }
    }
    return folds;
}

/// Collects folding ranges by walking the unit's cached Semantics node table —
/// the DFS pre-order record of the focus file's written AST — instead of
/// running another RecursiveASTVisitor over the TU, and by pairing the
/// brackets of a raw lex of the file. Folding needs no nesting state: every
/// recorded decl and stmt contributes its ranges independently.
///
/// Fold kinds are plain strings in the protocol (LSP standardizes only `comment`,
/// `imports` and `region`; servers may add custom values).
///
/// A delimited fold spans its delimiters, which `collapsed_text` repeats; a
/// section fold (an access-specifier section, a case of a switch, a
/// conditional branch, a region, a module fragment) runs from the end of
/// its header line to the next header, which stays visible; a run of line
/// comments or using declarations on consecutive lines, or of includes and
/// imports with only blank and comment lines between them, folds below its
/// first line.
class FoldingRangeCollector {
public:
    explicit FoldingRangeCollector(CompilationUnit& unit) :
        unit(unit), semantics(unit.get<Semantics>()), lexical(unit.focus_file().lexical()),
        content(unit.focus_file().content()) {
        for(auto& directive: lexical.directives) {
            if(directive.block()) {
                block_directives.push_back(directive.range.begin);
            }
        }
    }

    auto collect() -> std::vector<FoldingRange> {
        auto nodes = semantics.node_entries();
        std::uint32_t index = 0;
        while(index < nodes.size()) {
            const Semantics::Node& entry = nodes[index];
            if(!entry.node.is_ast()) {
                // The preprocessor segment follows the AST segment; directive
                // folds are collected from the lexical scan below.
                break;
            }

            if(entry.node.kind() == SemanticNode::Kind::Decl) {
                const auto* decl = entry.node.get<clang::Decl>();
                if(decls::is_instantiation(decl)) {
                    index = entry.subtree_end;
                    continue;
                }
                collect_decl(decl);
            } else if(entry.node.kind() == SemanticNode::Kind::Stmt) {
                collect_stmt(entry.node.get<clang::Stmt>());
            } else if(entry.node.kind() == SemanticNode::Kind::TypeLoc) {
                if(auto specialization = entry.node.get<clang::TypeLoc>()
                                             ->getAs<clang::TemplateSpecializationTypeLoc>()) {
                    add_template_arguments(specialization.getLAngleLoc(),
                                           specialization.getRAngleLoc());
                }
            }

            index += 1;
        }

        collect_brackets();
        collect_block_directives();
        collect_module_fragments(semantics.module_declarations());
        collect_comments(lexical.comments);
        collect_macro_definitions();
        collect_imports();
        collect_raw_strings(lexical.raw_strings);
        add_runs(usings, "usingDeclaration", false);

        return nest(std::move(candidates));
    }

private:
    void collect_decl(const clang::Decl* decl) {
        collect_template_parameters(decl);

        if(llvm::isa<clang::UsingDecl,
                     clang::UsingDirectiveDecl,
                     clang::UsingEnumDecl,
                     clang::UnresolvedUsingValueDecl,
                     clang::UnresolvedUsingTypenameDecl>(decl)) {
            // A declaration a macro produces spans the whole invocation,
            // which stands for every declaration it produces.
            if(auto local = unit.focus_file().produced_range(decl->getSourceRange());
               local && (usings.empty() || usings.back() != *local)) {
                usings.push_back(*local);
            }
            return;
        }

        if(const auto* import = llvm::dyn_cast<clang::ImportDecl>(decl)) {
            // `export import` puts the import in an export declaration.
            auto begin = import->getBeginLoc();
            if(const auto* exported = llvm::dyn_cast<clang::ExportDecl>(import->getDeclContext())) {
                begin = exported->getBeginLoc();
            }
            if(auto local = unit.focus_file().produced_range({begin, import->getEndLoc()})) {
                imports.push_back(*local);
            }
            return;
        }

        if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
            add_block(ns, ns->getRBraceLoc(), "namespace");
            return;
        }

        if(const auto* linkage = llvm::dyn_cast<clang::LinkageSpecDecl>(decl)) {
            if(linkage->hasBraces()) {
                add_block(linkage, linkage->getRBraceLoc(), "linkageSpec");
            }
            return;
        }

        if(const auto* exported = llvm::dyn_cast<clang::ExportDecl>(decl)) {
            if(exported->hasBraces()) {
                add_block(exported, exported->getRBraceLoc(), "export");
            }
            return;
        }

        if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(decl)) {
            if(!tag->isThisDeclarationADefinition()) {
                return;
            }

            add_declaration_block(tag->getBraceRange(),
                                  tag->getLocation(),
                                  *declaration_fold_kind(SymbolKind::from(tag)));

            if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(tag);
               record && !record->isLambda() && !record->isImplicit()) {
                collect_access_specifiers(record);
            }
            return;
        }

        // A deferred template body (-fdelayed-template-parsing) counts as a
        // body but is not there.
        if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
           function && function->doesThisDeclarationHaveABody() &&
           !function->isLateTemplateParsed()) {
            add_declaration_block(function->getBody()->getSourceRange(),
                                  function->getLocation(),
                                  *declaration_fold_kind(SymbolKind::from(function)));
        }
    }

    void collect_stmt(const clang::Stmt* stmt) {
        if(const auto* lambda = llvm::dyn_cast<clang::LambdaExpr>(stmt)) {
            if(!lambda->getExplicitTemplateParameters().empty()) {
                add_template_parameters(lambda->getTemplateParameterList());
            }
            return;
        }

        if(const auto* switch_stmt = llvm::dyn_cast<clang::SwitchStmt>(stmt)) {
            if(const auto* body = llvm::dyn_cast<clang::CompoundStmt>(switch_stmt->getBody())) {
                collect_cases(body);
            }
            return;
        }

        // A function's body folds first as its declaration's block; the
        // same braces again are dropped.
        if(llvm::isa<clang::CompoundStmt>(stmt)) {
            add_range(stmt->getSourceRange(), "compoundStmt", "{...}");
            return;
        }

        if(const auto* construct = llvm::dyn_cast<clang::CXXConstructExpr>(stmt)) {
            if(construct->isListInitialization()) {
                add_range(construct->getParenOrBraceRange(), "initializer", "{...}");
            }
            return;
        }

        if(const auto* init = llvm::dyn_cast<clang::InitListExpr>(stmt)) {
            add_range(clang::SourceRange(init->getLBraceLoc(), init->getRBraceLoc()),
                      "initializer",
                      "{...}");
            return;
        }

        // FIXME: the braces of a compound requirement spanning lines (`{ t.foo()
        // } -> C;`) do not fold: concepts::ExprRequirement records no brace
        // locations, and compiled code's braces fold from the AST alone.
        if(const auto* requirements = llvm::dyn_cast<clang::RequiresExpr>(stmt)) {
            // The opening brace is recorded nowhere; it follows the
            // parameter list, if any.
            auto range = requirements->getSourceRange();
            if(auto parameters_end = requirements->getRParenLoc(); parameters_end.isValid()) {
                range.setBegin(parameters_end);
            }
            auto tokens = unit.focus_file().tokens().expanded(range).drop_until(
                [](const clang::syntax::Token& token) {
                    return token.kind() == clang::tok::l_brace;
                });
            if(!tokens.empty()) {
                add_range(
                    clang::SourceRange(tokens.front().location(), requirements->getRBraceLoc()),
                    "requiresExpression",
                    "{...}");
            }
            return;
        }

        if(const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(stmt)) {
            add_template_arguments(reference->getLAngleLoc(), reference->getRAngleLoc());
        }
    }

    void collect_access_specifiers(const clang::CXXRecordDecl* record) {
        const clang::AccessSpecDecl* previous = nullptr;
        auto close = [&](clang::SourceLocation next) {
            if(!previous) {
                return;
            }
            auto header = unit.focus_file().produced_range(previous->getColonLoc());
            auto end = unit.focus_file().offset(next);
            if(header && end) {
                add_section(section_begin(header->end), *end, "accessSpecifier", Priority::Section);
            }
        };
        for(auto* member: record->decls()) {
            if(auto* access = llvm::dyn_cast<clang::AccessSpecDecl>(member)) {
                close(access->getAccessSpecifierLoc());
                previous = access;
            }
        }
        close(record->getBraceRange().getEnd());
    }

    /// Each case of a switch folds from its label to the next one, or to
    /// the switch's closing brace. A case holding nothing but a block
    /// opening on its label's line folds as that block.
    /// FIXME: only labels directly in the switch body fold: one nested in another
    /// statement (Duff's device, `if (c) { case 1: … }`) gets no section.
    void collect_cases(const clang::CompoundStmt* body) {
        struct Case {
            const clang::SwitchCase* label;
            /// Where the label begins, its attributes (`[[likely]]`) included.
            clang::SourceLocation begin;
            bool only_block;
        };

        auto label_of = [](const clang::Stmt* stmt) -> const clang::SwitchCase* {
            if(const auto* attributed = llvm::dyn_cast<clang::AttributedStmt>(stmt)) {
                stmt = attributed->getSubStmt();
            }
            return llvm::dyn_cast<clang::SwitchCase>(stmt);
        };

        llvm::SmallVector<Case> cases;
        for(const auto* child: body->body()) {
            const auto* label = label_of(child);
            if(!label) {
                if(!cases.empty()) {
                    cases.back().only_block = false;
                }
                continue;
            }
            // `case 1: case 2:` chains the labels each into the one before.
            for(const clang::Stmt* written = child; label;) {
                const auto* next = label_of(label->getSubStmt());
                cases.push_back({label,
                                 written->getBeginLoc(),
                                 !next && llvm::isa<clang::CompoundStmt>(label->getSubStmt())});
                written = label->getSubStmt();
                label = next;
            }
        }

        auto focus = unit.focus_file();
        for(auto [index, item]: llvm::enumerate(cases)) {
            auto next = index + 1 < cases.size() ? cases[index + 1].begin : body->getRBracLoc();
            auto colon = focus.produced_range(item.label->getColonLoc());
            auto end = focus.offset(next);
            if(!colon || !end) {
                continue;
            }
            auto begin = section_begin(colon->end);
            if(item.only_block && begin == colon->end) {
                continue;
            }
            add_section(begin, *end, "switchCase", Priority::Section);
        }
    }

    /// A template's own parameter list, and the outer lists an out-of-line
    /// member definition repeats for its enclosing templates.
    void collect_template_parameters(const clang::Decl* decl) {
        if(const auto* templated = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
            add_template_parameters(templated->getTemplateParameters());
        } else if(const auto* partial =
                      llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(decl)) {
            add_template_parameters(partial->getTemplateParameters());
        } else if(const auto* partial =
                      llvm::dyn_cast<clang::VarTemplatePartialSpecializationDecl>(decl)) {
            add_template_parameters(partial->getTemplateParameters());
        }

        llvm::ArrayRef<clang::TemplateParameterList*> outer;
        if(const auto* declarator = llvm::dyn_cast<clang::DeclaratorDecl>(decl)) {
            outer = declarator->getTemplateParameterLists();
        } else if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(decl)) {
            outer = tag->getTemplateParameterLists();
        }
        for(const auto* parameters: outer) {
            add_template_parameters(parameters);
        }
    }

    void add_template_parameters(const clang::TemplateParameterList* parameters) {
        add_range(clang::SourceRange(parameters->getLAngleLoc(), parameters->getRAngleLoc()),
                  "templateParams",
                  "<...>");
    }

    void add_template_arguments(clang::SourceLocation left, clang::SourceLocation right) {
        add_range(clang::SourceRange(left, right), "templateArgs", "<...>");
    }

    /// The parentheses and square brackets of the code, and every bracket
    /// pair of skipped code; not those of directive lines, as a macro's body
    /// pairs nothing written here. The braces of compiled code fold from the
    /// AST, which knows those a macro closes (`#define END }`).
    void collect_brackets() {
        auto& tokens = lexical.tokens;
        auto skipped = unit.focus_file().tokens().preprocessed_away();
        BracketPairs pairs;
        for(auto [token, skip]: llvm::zip_equal(tokens, skipped)) {
            auto open = pairs.visit(token, skip);
            if(!open || pairs.in_directive() ||
               (token.kind == clang::tok::r_brace && !pairs.in_skipped())) {
                continue;
            }
            // A pair filling another of its kind exactly, as `[[ ]]` and
            // `(( ))` do, folds as the outer one.
            if(!candidates.empty() && candidates.back().priority == Priority::Text &&
               candidates.back().fold.range == LocalSourceRange{open->end, token.range.begin} &&
               content[open->begin] == content[open->end]) {
                candidates.pop_back();
            }
            add_range(
                LocalSourceRange{open->begin, token.range.end},
                std::nullopt,
                std::format("{}...{}", content.slice(open->begin, open->end), token.text(content)),
                Priority::Text);
        }
    }

    /// The conditional branches and the regions.
    void collect_block_directives() {
        using enum LexicalInfo::Directive::Kind;
        llvm::SmallVector<const LexicalInfo::Directive*> branches;
        llvm::SmallVector<const LexicalInfo::Directive*> regions;
        for(const auto& directive: lexical.directives) {
            switch(directive.kind) {
                case If: branches.push_back(&directive); break;
                case Else:
                case EndIf: {
                    if(branches.empty()) {
                        break;
                    }
                    add_section(branches.back()->range.end,
                                directive.range.begin,
                                "conditionDirective",
                                Priority::Directive,
                                directive.kind == EndIf ? std::optional(directive.range.begin)
                                                        : std::nullopt);
                    if(directive.kind == Else) {
                        branches.back() = &directive;
                    } else {
                        branches.pop_back();
                    }
                    break;
                }
                case Region: regions.push_back(&directive); break;
                case EndRegion: {
                    if(!regions.empty()) {
                        add_section(regions.pop_back_val()->range.end,
                                    directive.range.begin,
                                    protocol::FoldingRangeKind::Region,
                                    Priority::Directive,
                                    directive.range.begin);
                    }
                    break;
                }
                default: break;
            }
        }
    }

    /// The global module fragment runs to the module declaration, the
    /// private one to the end of the file.
    void collect_module_fragments(llvm::ArrayRef<LexicalInfo::ModuleDeclaration> modules) {
        for(auto [index, module]: llvm::enumerate(modules)) {
            if(module.kind == LexicalInfo::ModuleDeclaration::Kind::Declaration) {
                continue;
            }
            auto end = static_cast<std::uint32_t>(content.size());
            if(index + 1 < modules.size()) {
                const auto& next = modules[index + 1];
                end = next.export_keyword.valid() ? next.export_keyword.begin : next.keyword.begin;
            }
            add_section(line_text_end(module.keyword.begin),
                        end,
                        "moduleFragment",
                        Priority::Section);
        }
    }

    void collect_comments(llvm::ArrayRef<LexicalInfo::Comment> comments) {
        llvm::SmallVector<LocalSourceRange> line_comments;
        for(const auto& comment: comments) {
            if(comment.kind == LexicalInfo::Comment::Kind::Line) {
                line_comments.push_back(comment.range);
            } else if(begins_line(comment.range.begin)) {
                // A block comment trailing code would cut across the folds
                // that start at the end of that code's line.
                add_range(comment.range,
                          protocol::FoldingRangeKind::Comment,
                          "/*...*/",
                          Priority::Text);
            }
        }
        add_runs(line_comments, protocol::FoldingRangeKind::Comment, false);
    }

    /// A definition continued over several lines folds below its first
    /// line, like a run.
    void collect_macro_definitions() {
        for(auto& directive: lexical.directives) {
            if(directive.kind != LexicalInfo::Directive::Kind::Define) {
                continue;
            }
            auto definition = directive.range;
            auto first_line_end = line_text_end(definition.begin);
            if(first_line_end < definition.end) {
                candidates.push_back({
                    .fold = {.range = {first_line_end, line_text_end(definition.end)},
                             .kind = "macroDefinition",
                             .lines = definition},
                    .priority = Priority::Text,
                });
            }
        }
    }

    /// Include directives and import declarations fold as one run, blank
    /// and comment lines between them included.
    void collect_imports() {
        for(auto& directive: lexical.directives) {
            if(directive.kind == LexicalInfo::Directive::Kind::Include) {
                imports.push_back(directive.range);
            }
        }
        std::ranges::sort(imports, {}, &LocalSourceRange::begin);
        add_runs(imports, protocol::FoldingRangeKind::Imports, true);
    }

    /// A raw string folds on its delimiters, which may carry a custom
    /// delimiter, an encoding prefix and a literal suffix.
    void collect_raw_strings(llvm::ArrayRef<LocalSourceRange> literals) {
        for(auto literal: literals) {
            auto text = content.substr(literal.begin, literal.length());
            add_range(literal,
                      "rawString",
                      std::format("{}...{}",
                                  text.take_front(text.find('(') + 1),
                                  text.drop_front(text.rfind(')'))),
                      Priority::Text);
        }
    }

    /// A brace block whose declaration records only its closing brace: the
    /// opening one is the declaration's first `{`.
    void add_block(const clang::Decl* decl,
                   clang::SourceLocation right_brace,
                   protocol::FoldingRangeKind kind) {
        auto tokens = unit.focus_file()
                          .tokens()
                          .expanded(decl->getSourceRange())
                          .drop_until([](const clang::syntax::Token& token) {
                              return token.kind() == clang::tok::l_brace;
                          });
        if(!tokens.empty()) {
            add_declaration_block(clang::SourceRange(tokens.front().location(), right_brace),
                                  decl->getBeginLoc(),
                                  std::move(kind));
        }
    }

    /// `head` is the declaration's name, or the keyword opening a
    /// namespace, linkage or export block (declaration_lines).
    void add_declaration_block(clang::SourceRange braces,
                               clang::SourceLocation head,
                               protocol::FoldingRangeKind kind) {
        auto* fold = add_range(braces, std::move(kind), "{...}");
        if(!fold) {
            return;
        }
        if(auto offset = unit.focus_file().offset(head)) {
            fold->lines = declaration_lines(content, fold->range, *offset, block_directives);
        }
    }

    /// The fold added, if any.
    FoldingRange* add_range(clang::SourceRange range,
                            protocol::FoldingRangeKind kind,
                            llvm::StringRef collapsed_text) {
        if(range.isInvalid()) {
            return nullptr;
        }

        // What macro arguments spell folds where it is written; what a
        // macro body produces folds at the invocation.
        auto& SM = unit.context().getSourceManager();
        auto begin = SM.getFileLoc(range.getBegin());
        auto end = SM.getFileLoc(range.getEnd());
        if(begin == end) {
            return nullptr;
        }

        // A block an included file closes folds to the end of this one.
        auto focus = unit.focus_file();
        auto local = focus.produced_range({begin, end});
        if(!local) {
            return nullptr;
        }

        // A delimiter a macro writes stays visible with its invocation; one
        // an included file writes is not in the fold.
        auto [head, tail] = collapsed_text.split("...");
        if(!content.substr(local->begin).starts_with(head)) {
            local->begin = focus.produced_range(range.getBegin())->end;
            head = "";
        }
        if(!content.substr(0, local->end).ends_with(tail)) {
            if(auto closing = focus.produced_range(range.getEnd())) {
                local->end = closing->begin;
            }
            tail = "";
        }
        if(local->end <= local->begin) {
            return nullptr;
        }
        return add_range(*local,
                         std::move(kind),
                         std::format("{}...{}", head, tail),
                         Priority::Syntax);
    }

    FoldingRange* add_range(LocalSourceRange range,
                            std::optional<protocol::FoldingRangeKind> kind,
                            std::string collapsed_text,
                            Priority priority) {
        // Single-line ranges are not worth folding.
        if(!content.substr(range.begin, range.length()).contains('\n')) {
            return nullptr;
        }

        std::optional<std::uint32_t> closer;
        auto tail = llvm::StringRef(collapsed_text).split("...").second;
        if(!tail.empty() && content.substr(0, range.end).ends_with(tail)) {
            closer = range.end - tail.size();
        }

        return &candidates
                    .emplace_back(Candidate{
                        .fold = {.range = range,
                                 .kind = std::move(kind),
                                 .collapsed_text = std::move(collapsed_text),
                                 .closer = closer},
                        .priority = priority,
        })
                    .fold;
    }

    /// Folds each run of two or more items below the run's first line.
    /// Only an item alone on its lines, trailed by nothing but its `;` and
    /// a line comment, joins a run, and only from the next line — or,
    /// `across_blanks`, from any line after blank and comment lines. Nothing
    /// closes a run on its last line, so a line-folding client hides that
    /// line too.
    void add_runs(llvm::ArrayRef<LocalSourceRange> items,
                  protocol::FoldingRangeKind kind,
                  bool across_blanks) {
        std::optional<LocalSourceRange> first;
        LocalSourceRange last;
        auto flush = [&] {
            if(first && last != *first) {
                auto end = line_text_end(last.end);
                candidates.push_back({
                    .fold = {.range = {line_text_end(first->begin), end},
                             .kind = kind,
                             .lines = LocalSourceRange{first->begin, end}},
                    .priority = Priority::Text,
                });
            }
        };

        for(auto item: items) {
            auto rest = content.slice(item.end, line_text_end(item.end)).ltrim(" \t;");
            if(!begins_line(item.begin) || !(rest.empty() || rest.starts_with("//"))) {
                continue;
            }
            if(first) {
                LocalSourceRange gap{line_end(content, last.end), line_begin(content, item.begin)};
                if(gap.begin == gap.end || (across_blanks && blank(gap))) {
                    last = item;
                    continue;
                }
            }
            flush();
            first = last = item;
        }
        flush();
    }

    /// Whether `range` holds nothing but blanks and comments.
    bool blank(LocalSourceRange range) {
        const auto& comments = lexical.comments;
        auto comment = std::ranges::lower_bound(
            comments,
            range.begin,
            {},
            [](const LexicalInfo::Comment& comment) { return comment.range.begin; });
        auto begin = range.begin;
        for(; comment != comments.end() && comment->range.end <= range.end; ++comment) {
            if(!content.slice(begin, comment->range.begin).trim().empty()) {
                return false;
            }
            begin = comment->range.end;
        }
        return content.slice(begin, range.end).trim().empty();
    }

    /// Whether only blanks precede `offset` on its line.
    bool begins_line(std::uint32_t offset) {
        return content.slice(line_begin(content, offset), offset).trim(" \t").empty();
    }

    /// Where a section under a label ending at `label_end` begins: at the
    /// end of the label's line, or right after the label when code follows
    /// it there (`public: void f() {`) — the code's own folds start on that
    /// line too.
    std::uint32_t section_begin(std::uint32_t label_end) {
        auto end = line_text_end(label_end);
        auto rest = content.slice(label_end, end).ltrim(" \t");
        while(rest.starts_with("/*") && rest.contains("*/")) {
            rest = rest.split("*/").second.ltrim(" \t");
        }
        return rest.empty() || rest.starts_with("//") || rest.starts_with("/*") ? end : label_end;
    }

    /// `begin` ends a header line; a section hiding no whole line is noise.
    /// One the end of the file closes hides through its last line, there
    /// being no next header to keep visible.
    void add_section(std::uint32_t begin,
                     std::uint32_t end,
                     protocol::FoldingRangeKind kind,
                     Priority priority,
                     std::optional<std::uint32_t> closer = std::nullopt) {
        if(end <= begin || content.substr(begin, end - begin).count('\n') < 2) {
            return;
        }
        std::optional<LocalSourceRange> lines;
        if(end == content.size()) {
            lines = LocalSourceRange{begin, static_cast<std::uint32_t>(content.rtrim().size())};
        }
        candidates.push_back({
            .fold =
                {.range = {begin, end}, .kind = std::move(kind), .lines = lines, .closer = closer},
            .priority = priority,
        });
    }

    /// Where the text of the line holding `offset` ends.
    std::uint32_t line_text_end(std::uint32_t offset) {
        return static_cast<std::uint32_t>(
            std::min(content.find_first_of("\r\n", offset), content.size()));
    }

    CompilationUnit& unit;
    const Semantics& semantics;
    const LexicalInfo& lexical;
    llvm::StringRef content;
    std::vector<Candidate> candidates;

    /// Where each conditional or region directive begins, in source order.
    std::vector<std::uint32_t> block_directives;

    /// Using declarations and directives met on the walk, in source order.
    llvm::SmallVector<LocalSourceRange> usings;

    /// Import declarations met on the walk, in source order.
    llvm::SmallVector<LocalSourceRange> imports;
};

}  // namespace

auto folding_ranges(CompilationUnit& unit) -> std::vector<FoldingRange> {
    return FoldingRangeCollector(unit).collect();
}

auto declaration_fold_kind(SymbolKind kind) -> std::optional<protocol::FoldingRangeKind> {
    switch(kind) {
        case SymbolKind::Namespace: return "namespace";
        case SymbolKind::Class: return "class";
        case SymbolKind::Struct: return "struct";
        case SymbolKind::Union: return "union";
        case SymbolKind::Enum: return "enum";
        case SymbolKind::Function:
        case SymbolKind::Method:
        case SymbolKind::Operator: return "functionBody";
        default: return std::nullopt;
    }
}

auto declaration_lines(llvm::StringRef content,
                       LocalSourceRange block,
                       std::uint32_t head,
                       llvm::ArrayRef<std::uint32_t> block_directives)
    -> std::optional<LocalSourceRange> {
    if(content.slice(block.begin, block.end).count('\n') < 2 ||
       line_begin(content, head) >= line_begin(content, block.begin)) {
        return std::nullopt;
    }
    auto next = std::ranges::lower_bound(block_directives, head);
    if(next != block_directives.end() && *next <= block.begin) {
        return std::nullopt;
    }
    return LocalSourceRange{head, line_begin(content, block.end) - 1};
}

namespace {

/// A fold as a client folding whole lines sees it.
struct LineFold {
    const FoldingRange* item;
    std::uint32_t start;
    std::uint32_t end;

    /// The line of the fold's closer when only blanks and other closers
    /// precede it there: the fold may hide that line too.
    std::optional<std::uint32_t> closer_line;

    bool joined = false;
};

/// The text of the line holding `offset`, without its line break.
llvm::StringRef line_text(llvm::StringRef content, std::uint32_t offset) {
    return content.slice(line_begin(content, offset), line_end(content, offset)).rtrim("\r\n");
}

/// What a client showing a fold's collapsed text shows for it: its first
/// line, the opening brace when that sits alone on the next line, `...`
/// for what it hides, and the line of its closer when it folds that line
/// too.
std::string line_collapsed_text(llvm::StringRef content, const LineFold& fold) {
    const auto& range = fold.item->range;
    auto first_line = fold.item->lines.value_or(range).begin;
    std::string text = line_text(content, first_line).rtrim().str();
    auto opener_line = line_text(content, range.begin).trim();
    if(line_begin(content, range.begin) == line_end(content, first_line) &&
       opener_line == content.substr(range.begin, 1)) {
        text += ' ';
        text += opener_line;
    }
    text += "...";
    if(fold.joined) {
        text += line_text(content, *fold.item->closer).trim();
    }
    return text;
}

}  // namespace

auto folding_ranges_to_protocol(llvm::ArrayRef<FoldingRange> ranges,
                                llvm::StringRef content,
                                const PositionMap& map,
                                PositionEncoding encoding,
                                const FoldingClient& client)
    -> std::vector<protocol::FoldingRange> {
    std::vector<protocol::FoldingRange> result;
    if(!client.line_folding_only) {
        result.reserve(ranges.size());
        for(const auto& item: ranges) {
            auto start = map.position(item.range.begin, encoding);
            auto end = map.position(item.range.end, encoding);
            if(!start || !end) {
                continue;
            }
            protocol::FoldingRange range{
                .start_line = start->line,
                .start_character = start->character,
                .end_line = end->line,
                .end_character = end->character,
            };
            if(item.kind) {
                range.kind = *item.kind;
            }
            if(!item.collapsed_text.empty()) {
                range.collapsed_text = item.collapsed_text;
            }
            result.push_back(std::move(range));
        }
        return result;
    }

    auto line_of = [&](std::uint32_t offset) -> std::optional<std::uint32_t> {
        auto position = map.position(offset, encoding);
        return position ? std::optional(position->line) : std::nullopt;
    };

    // The client hides whole lines below the start line. The line a fold's
    // range ends on holds its closing delimiter or the next header —
    // `} else {`, `#else`, `private:` — and stays visible; `lines` ends on
    // its last hidden line instead.
    std::vector<LineFold> folds;
    llvm::DenseSet<std::uint32_t> starts;
    for(const auto& item: ranges) {
        auto bounds = item.lines.value_or(item.range);
        auto start = line_of(bounds.begin);
        auto end = line_of(bounds.end);
        if(!start || !end) {
            continue;
        }
        LineFold fold{.item = &item, .start = *start, .end = item.lines ? *end : *end - 1};
        if(client.collapsed_text && item.closer &&
           content.slice(line_begin(content, *item.closer), *item.closer)
               .ltrim(" \t)]}>")
               .empty()) {
            fold.closer_line = line_of(*item.closer);
        }
        starts.insert(fold.start);
        folds.push_back(fold);
    }

    // A client showing the collapsed text folds the closer's line too,
    // `int f() {...}`, when nothing but other closers precedes the closer
    // there — unless the line opens another fold, `} else {`, or a fold
    // around this one ends before it: a lambda closing in `}, rest);` would
    // cross the call around it.
    std::vector<LineFold*> order;
    for(auto& fold: folds) {
        order.push_back(&fold);
    }
    std::ranges::stable_sort(order, [](const LineFold* lhs, const LineFold* rhs) {
        return std::tuple(lhs->item->range.begin, rhs->item->range.end) <
               std::tuple(rhs->item->range.begin, lhs->item->range.end);
    });
    llvm::SmallVector<const LineFold*> around;
    for(auto* fold: order) {
        while(!around.empty() && around.back()->item->range.end <= fold->item->range.begin) {
            around.pop_back();
        }
        auto line = fold->closer_line;
        if(line && !starts.contains(*line) &&
           llvm::none_of(around, [&](const LineFold* outer) { return outer->end + 1 == *line; })) {
            fold->end = *line;
            fold->joined = true;
        }
        around.push_back(fold);
    }
    std::erase_if(folds, [](const LineFold& fold) { return fold.end <= fold.start; });

    // VS Code keeps only the first fold it sees starting on a line. A body
    // starting on its declaration's line must win over the parameter list
    // folding from that line, so outer folds go first; of folds hiding the
    // same lines, as a call's and the lambda closing it on one line do,
    // one stays.
    std::ranges::stable_sort(folds, [](const LineFold& lhs, const LineFold& rhs) {
        return std::tie(lhs.start, rhs.end) < std::tie(rhs.start, lhs.end);
    });
    auto same = std::ranges::unique(folds, [](const LineFold& lhs, const LineFold& rhs) {
        return lhs.start == rhs.start && lhs.end == rhs.end;
    });
    folds.erase(same.begin(), same.end());

    result.reserve(folds.size());
    for(const auto& fold: folds) {
        protocol::FoldingRange range{.start_line = fold.start, .end_line = fold.end};
        if(fold.item->kind) {
            range.kind = *fold.item->kind;
        }
        if(client.collapsed_text) {
            range.collapsed_text = line_collapsed_text(content, fold);
        }
        result.push_back(std::move(range));
    }
    return result;
}

}  // namespace clice::feature
