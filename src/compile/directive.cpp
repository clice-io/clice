module clice;

import :compile.compilation;
import :compile.directive;
import :syntax.lexer;
import :syntax.scan;

namespace clice {

class CompilationUnit::DirectiveRecorder : public clang::PPCallbacks {
public:
    DirectiveRecorder(CompilationUnit& unit) : unit(unit) {}

private:
    void add_condition(clang::SourceLocation location,
                       Condition::BranchKind kind,
                       Condition::ConditionValue value) {
        auto& directive = unit.collected_directives[unit.file_id(location)];
        directive.conditions.emplace_back(kind, value, location);
    }

    void add_condition(clang::SourceLocation loc,
                       Condition::BranchKind kind,
                       clang::PPCallbacks::ConditionValueKind value) {
        Condition::ConditionValue cond_value =
            value == clang::PPCallbacks::CVK_False          ? Condition::False
            : value == clang::PPCallbacks::CVK_True         ? Condition::True
            : value == clang::PPCallbacks::CVK_NotEvaluated ? Condition::Skipped
                                                            : Condition::None;
        add_condition(loc, kind, cond_value);
    }

    /// `negated` flips the recorded truth for #ifndef/#elifndef: the
    /// stored value is the BRANCH truth (was it taken), not whether the
    /// macro is defined — an include guard's first pass is active.
    void add_condition(clang::SourceLocation loc,
                       Condition::BranchKind kind,
                       const clang::Token& name,
                       const clang::MacroDefinition& definition,
                       bool negated = false) {
        auto def = definition.getMacroInfo();
        if(def) {
            add_macro(def, MacroRef::Ref, name.getLocation());
        } else {
            add_unresolved(name.getLocation());
        }
        bool taken = negated ? def == nullptr : def != nullptr;
        add_condition(loc, kind, taken ? Condition::True : Condition::False);
    }

    void add_macro(const clang::MacroInfo* def, MacroRef::Kind kind, clang::SourceLocation loc) {
        if(def->isBuiltinMacro()) {
            return;
        }

        auto written = argument_spelling(loc);
        // What <built-in> and <command line> (`-D`) define and name lies in
        // no file: such a macro has its uses alone, and the index tells
        // where it comes from by its entity (EntityTable::predefined_macro).
        if(unit.is_builtin_file(unit.file_id(written))) {
            return;
        }
        // A body using its argument twice expands the name twice.
        if(written != loc && !written_arguments.insert({def, written}).second) {
            return;
        }

        auto& directive = unit.collected_directives[unit.file_id(written)];
        directive.macros.emplace_back(MacroRef{def, kind, written});
    }

    /// Where a macro name written as another macro's argument and invoked
    /// by its body (`APPLY(F)` for `F()`) is spelled; any other location as
    /// it is — a name of a body is the expansions' to resolve, and a
    /// pasted name is spelled nowhere.
    clang::SourceLocation argument_spelling(clang::SourceLocation location) {
        auto& SM = unit.SM();
        auto spelling = location;
        clang::SourceLocation last;
        while(spelling.isMacroID()) {
            last = spelling;
            spelling = SM.getImmediateSpellingLoc(spelling);
        }
        if(last.isValid() &&
           SM.getSLocEntry(SM.getFileID(last)).getExpansion().isMacroArgExpansion()) {
            return spelling;
        }
        return location;
    }

    clang::MacroInfo* definition(llvm::StringRef name) {
        auto& pp = unit.instance->getPreprocessor();
        auto& identifiers = pp.getIdentifierTable();
        auto known = identifiers.find(name);
        return known == identifiers.end() ? nullptr : pp.getMacroInfo(known->getValue());
    }

    /// A system header's directives test what its platform and the user
    /// configure: none is a name of the project.
    void add_unresolved(clang::SourceLocation location) {
        if(!unit.in_system_header(location)) {
            unit.collected_directives[unit.file_id(location)].unresolved_macros.push_back(location);
        }
    }

    /// The tokens of `fid` from `begin` to `end`.
    std::vector<Token> tokens_of(clang::FileID fid, std::uint32_t begin, std::uint32_t end) {
        Lexer lexer(unit.file_content(fid).substr(begin), unit.lang_options());
        std::vector<Token> tokens;
        for(auto token = lexer.advance(); !token.is_eof() && begin + token.range.begin < end;
            token = lexer.advance()) {
            token.range.begin += begin;
            token.range.end += begin;
            tokens.push_back(token);
        }
        return tokens;
    }

    /// The macro names of a condition expression no definition resolves:
    /// its identifiers but `defined`, the keywords and the builtin macros
    /// with their arguments (`__has_include(<x>)`). Of a condition the
    /// preprocessor `evaluated`, those it found defined are left out, their
    /// expansions recorded them, as is `defined`'s operand, which Defined
    /// sees. Of one it did not, a name defined there references that
    /// definition (add_tested).
    void add_unresolved(clang::FileID fid, llvm::ArrayRef<Token> condition, bool evaluated) {
        auto& identifiers = unit.instance->getPreprocessor().getIdentifierTable();
        auto content = unit.file_content(fid);
        for(std::size_t i = 0; i < condition.size(); i += 1) {
            auto& token = condition[i];
            if(!token.is_identifier() ||
               keyword_kind(token.text(content), identifiers) != clang::tok::identifier) {
                continue;
            }
            auto name = token.text(content);
            if(name == "defined") {
                if(i + 1 < condition.size() && condition[i + 1].kind == clang::tok::l_paren) {
                    i += 1;
                }
                if(!evaluated && i + 1 < condition.size() && condition[i + 1].is_identifier()) {
                    add_tested(fid, condition[i + 1], content);
                }
                i += 1;
                continue;
            }
            auto* macro = definition(name);
            if(macro && macro->isBuiltinMacro()) {
                if(i + 1 < condition.size() && condition[i + 1].kind == clang::tok::l_paren) {
                    std::uint32_t depth = 0;
                    for(i += 1; i < condition.size(); i += 1) {
                        if(condition[i].kind == clang::tok::l_paren) {
                            depth += 1;
                        } else if(condition[i].kind == clang::tok::r_paren) {
                            depth -= 1;
                            if(depth == 0) {
                                break;
                            }
                        }
                    }
                }
                continue;
            }
            if(!evaluated) {
                add_tested(fid, token, content);
            } else if(!macro) {
                add_unresolved(unit.create_location(fid, token.range.begin));
            }
        }
    }

    /// A macro name a condition the preprocessor did not evaluate tests: it
    /// references the definition the name has there, the preprocessor's
    /// still, since no directive of a skipped block runs.
    void add_tested(clang::FileID fid, const Token& name, llvm::StringRef content) {
        auto location = unit.create_location(fid, name.range.begin);
        if(auto* macro = definition(name.text(content))) {
            add_macro(macro, MacroRef::Ref, location);
        } else {
            add_unresolved(location);
        }
    }

    void add_unresolved(clang::SourceRange condition, bool evaluated) {
        if(condition.isInvalid() || unit.in_system_header(condition.getBegin())) {
            return;
        }
        auto [fid, range] = unit.produced_range(condition);
        add_unresolved(fid, tokens_of(fid, range.begin, range.end), evaluated);
    }

public:
    /// ============================================================================
    ///                         Rewritten Preprocessor Callbacks
    /// ============================================================================

    void HasEmbed(clang::SourceLocation location,
                  llvm::StringRef filename,
                  bool is_angled,
                  clang::OptionalFileEntryRef file) override {
        unit.collected_directives[unit.file_id(location)].has_embeds.emplace_back(clice::HasEmbed{
            .file_name = filename,
            .file = file,
            .is_angled = is_angled,
            .loc = location,
        });
    }

    void EmbedDirective(clang::SourceLocation location,
                        clang::StringRef filename,
                        bool is_angled,
                        clang::OptionalFileEntryRef file,
                        const clang::LexEmbedParametersResult&) override {
        unit.collected_directives[unit.file_id(location)].embeds.emplace_back(Embed{
            .file_name = filename,
            .file = file,
            .is_angled = is_angled,
            .loc = location,
        });
    }

    /// A lookup that found nothing still consulted the disk: every place it
    /// looked is an absent input.
    void add_absent(llvm::StringRef name, bool angled, clang::SourceLocation location) {
        lookup_places(unit.instance->getFileManager(),
                      unit.instance->getPreprocessor().getHeaderSearchInfo(),
                      unit.SM().getFileEntryRefForID(unit.file_id(location)),
                      name,
                      angled,
                      [&](std::string place) { unit.absent_paths.insert(place); });
    }

    void InclusionDirective(clang::SourceLocation hash_loc,
                            const clang::Token& include_tok,
                            llvm::StringRef file_name,
                            bool is_angled,
                            clang::CharSourceRange,
                            clang::OptionalFileEntryRef file,
                            llvm::StringRef,
                            llvm::StringRef,
                            const clang::Module*,
                            bool,
                            clang::SrcMgr::CharacteristicKind) override {
        prev_fid = unit.file_id(hash_loc);
        if(!file) {
            add_absent(file_name, is_angled, hash_loc);
        }

        /// An `IncludeDirective` call is always followed by either a `LexedFileChanged`
        /// or a `FileSkipped`. so we cannot get the file id of included file here.
        unit.collected_directives[prev_fid].includes.emplace_back(Include{
            .fid = {},
            .location = include_tok.getLocation(),
            .macro_braces = macro_braces.lookup(prev_fid),
        });
    }

    void LexedFileChanged(clang::FileID curr_fid,
                          LexedFileChangeReason reason,
                          clang::SrcMgr::CharacteristicKind,
                          clang::FileID prev_fid,
                          clang::SourceLocation) override {
        if(reason != LexedFileChangeReason::EnterFile || curr_fid.isInvalid()) {
            return;
        }
        if(auto entry = unit.SM().getFileEntryRefForID(curr_fid)) {
            unit.file_ids.try_emplace(&entry->getFileEntry(), curr_fid);
        }
        if(prev_fid.isValid() && this->prev_fid.isValid() && prev_fid == this->prev_fid) {
            /// Once the file has changed, it means that the last include is not skipped.
            /// Therefore, we initialize its file id with the current file id.
            auto& include = unit.collected_directives[prev_fid].includes.back();
            include.skipped = false;
            include.fid = curr_fid;
        }
    }

    void FileSkipped(const clang::FileEntryRef& file,
                     const clang::Token&,
                     clang::SrcMgr::CharacteristicKind) override {
        if(prev_fid.isValid()) {
            /// File with guard will have only one file id in `SourceManager`.
            auto& include = unit.collected_directives[prev_fid].includes.back();
            include.skipped = true;

            /// Get the FileID for the given file. If the source file is included multiple
            /// times, the FileID will be the first inclusion.
            include.fid = unit.file_id(file);
        }
    }

    void moduleImport(clang::SourceLocation import_location,
                      clang::ModuleIdPath names,
                      const clang::Module* M) override {
        auto fid = unit.produced_at(import_location).file;
        auto& import = unit.collected_directives[fid].imports.emplace_back();
        import.location = import_location;
        for(auto name: names) {
            if(!import.name.empty())
                import.name += '.';
            import.name += name.getIdentifierInfo()->getName();
        }
        import.full_name = M ? M->getFullModuleName() : import.name;

        // Clang reports a C++20 module name flattened into one component
        // at its first token — a partition's at its colon — so the written
        // identifiers are lexed back from there.
        // FIXME: when only the first segment comes from a macro
        // (`import ROOT.bar;`), the written `.bar` past it gets no location:
        // no token, highlight or jump.
        auto start = names.front().getLoc();
        if(start.isMacroID()) {
            import.name_locations.push_back(start);
            return;
        }
        auto [name_fid, offset] = unit.written_at(start);
        Lexer lexer(unit.file_content(name_fid).substr(offset), unit.lang_options());
        auto token = lexer.advance();
        if(token.kind == clang::tok::colon) {
            token = lexer.advance();
        }
        while(token.is_identifier()) {
            import.name_locations.push_back(
                unit.create_location(name_fid, offset + token.range.begin));
            if(lexer.advance().kind != clang::tok::period) {
                break;
            }
            token = lexer.advance();
        }
    }

    void HasInclude(clang::SourceLocation location,
                    llvm::StringRef file_name,
                    bool is_angled,
                    clang::OptionalFileEntryRef file,
                    clang::SrcMgr::CharacteristicKind) override {
        // The filename may come from a macro's argument: record where it
        // is spelled.
        location = unit.SM().getFileLoc(location);
        unit.collected_directives[unit.file_id(location)].has_includes.emplace_back(file, location);
        if(!file) {
            add_absent(file_name, is_angled, location);
        }
    }

    void PragmaDirective(clang::SourceLocation loc, clang::PragmaIntroducerKind kind) override {
        if(kind != clang::PIK_HashPragma) {
            auto fid = unit.produced_at(loc).file;
            unit.collected_directives[fid].pragma_operators.push_back(loc);
        }
    }

    void PragmaDebug(clang::SourceLocation, llvm::StringRef command) override {
        // `dump` leaves the rest of its line to the parser, which prints
        // it. Nothing here wants the dump printed.
        if(command == "dump") {
            unit.instance->getPreprocessor().DiscardUntilEndOfDirective();
        }
    }

    void PragmaDiagnosticPush(clang::SourceLocation loc, llvm::StringRef) override {
        add_diagnostic_pragma({.kind = DiagnosticPragma::Push, .loc = loc});
    }

    void PragmaDiagnosticPop(clang::SourceLocation loc, llvm::StringRef) override {
        add_diagnostic_pragma({.kind = DiagnosticPragma::Pop, .loc = loc});
    }

    void PragmaDiagnostic(clang::SourceLocation loc,
                          llvm::StringRef,
                          clang::diag::Severity severity,
                          llvm::StringRef flag) override {
        add_diagnostic_pragma(
            {.kind = DiagnosticPragma::Map, .severity = severity, .flag = flag.str(), .loc = loc});
    }

    void add_diagnostic_pragma(DiagnosticPragma pragma) {
        auto fid = unit.produced_at(pragma.loc).file;
        unit.collected_directives[fid].diagnostic_pragmas.push_back(std::move(pragma));
    }

    void If(clang::SourceLocation loc,
            clang::SourceRange condition,
            clang::PPCallbacks::ConditionValueKind value) override {
        add_unresolved(condition, /*evaluated=*/true);
        add_condition(loc, Condition::If, value);
    }

    void Elif(clang::SourceLocation loc,
              clang::SourceRange condition,
              clang::PPCallbacks::ConditionValueKind value,
              clang::SourceLocation) override {
        add_unresolved(condition, value != clang::PPCallbacks::CVK_NotEvaluated);
        add_condition(loc, Condition::Elif, value);
    }

    void Ifdef(clang::SourceLocation loc,
               const clang::Token& name,
               const clang::MacroDefinition& definition) override {
        add_condition(loc, Condition::Ifdef, name, definition);
    }

    /// Invoke when #elifdef branch is taken.
    void Elifdef(clang::SourceLocation loc,
                 const clang::Token& name,
                 const clang::MacroDefinition& definition) override {
        add_condition(loc, Condition::Elifdef, name, definition);
    }

    /// Invoke when #elif is skipped.
    void Elifdef(clang::SourceLocation loc,
                 clang::SourceRange condition,
                 clang::SourceLocation) override {
        add_unresolved(condition, /*evaluated=*/false);
        add_condition(loc, Condition::Elifdef, Condition::Skipped);
    }

    /// Invoke when #ifndef is taken.
    void Ifndef(clang::SourceLocation loc,
                const clang::Token& name,
                const clang::MacroDefinition& definition) override {
        add_condition(loc, Condition::Ifndef, name, definition, /*negated=*/true);
    }

    // Invoke when #elifndef is taken.
    void Elifndef(clang::SourceLocation loc,
                  const clang::Token& name,
                  const clang::MacroDefinition& definition) override {
        add_condition(loc, Condition::Elifndef, name, definition, /*negated=*/true);
    }

    // Invoke when #elifndef is skipped.
    void Elifndef(clang::SourceLocation loc,
                  clang::SourceRange condition,
                  clang::SourceLocation) override {
        add_unresolved(condition, /*evaluated=*/false);
        add_condition(loc, Condition::Elifndef, Condition::Skipped);
    }

    /// The macro names of the directives a skipped block holds that no
    /// callback reported: the block's own opening and closing lines and an
    /// `#elif` the preprocessor evaluated in it are reported as conditions.
    void SourceRangeSkipped(clang::SourceRange range, clang::SourceLocation) override {
        if(unit.in_system_header(range.getBegin())) {
            return;
        }
        auto [fid, begin] = unit.decompose(range.getBegin());
        auto end = unit.decompose(range.getEnd()).offset;
        auto& directive = unit.collected_directives[fid];
        llvm::DenseSet<std::uint32_t> reported;
        for(auto& condition: views::reverse(directive.conditions)) {
            auto offset = unit.decompose(condition.loc).offset;
            if(offset < begin) {
                break;
            }
            reported.insert(offset);
        }
        auto content = unit.file_content(fid);
        auto tokens = tokens_of(fid, begin, end);
        for(std::size_t i = 0; i < tokens.size(); i += 1) {
            if(!tokens[i].is_directive_hash() || i + 1 == tokens.size()) {
                continue;
            }
            auto& keyword = tokens[i + 1];
            auto line = i + 2;
            while(i + 1 < tokens.size() && !tokens[i + 1].is_eod()) {
                i += 1;
            }
            if(!keyword.is_pp_keyword || reported.contains(keyword.range.begin)) {
                continue;
            }
            auto spelled = llvm::ArrayRef(tokens).slice(line, i + 1 - line);
            auto name = keyword.text(content);
            if(name == "if" || name == "elif") {
                add_unresolved(fid, spelled, /*evaluated=*/false);
            } else if(spelled.empty() || !spelled.front().is_identifier()) {
                continue;
            } else if(name == "define" || name == "undef") {
                add_unresolved(unit.create_location(fid, spelled.front().range.begin));
            } else if(ranges::contains(std::array{"ifdef", "ifndef", "elifdef", "elifndef"},
                                       name)) {
                add_tested(fid, spelled.front(), content);
            }
        }
    }

    void Else(clang::SourceLocation loc, clang::SourceLocation if_loc) override {
        add_condition(loc, Condition::Else, Condition::None);
    }

    void Endif(clang::SourceLocation loc, clang::SourceLocation if_loc) override {
        add_condition(loc, Condition::EndIf, Condition::None);
    }

    void MacroDefined(const clang::Token& name, const clang::MacroDirective* md) override {
        if(auto def = md->getMacroInfo()) {
            add_macro(def, MacroRef::Def, name.getLocation());
        }
    }

    void MacroExpands(const clang::Token& name,
                      const clang::MacroDefinition& definition,
                      clang::SourceRange range,
                      const clang::MacroArgs* args) override {
        auto def = definition.getMacroInfo();
        if(!def) {
            return;
        }
        add_macro(def, MacroRef::Ref, name.getLocation());
        std::int32_t braces = 0;
        for(auto& token: def->tokens()) {
            braces += token.is(clang::tok::l_brace) ? 1 : token.is(clang::tok::r_brace) ? -1 : 0;
        }
        if(braces != 0) {
            macro_braces[unit.produced_at(name.getLocation()).file] += braces;
        }
    }

    void Defined(const clang::Token& name,
                 const clang::MacroDefinition& definition,
                 clang::SourceRange) override {
        if(auto def = definition.getMacroInfo()) {
            add_macro(def, MacroRef::Ref, name.getLocation());
        } else if(auto written = argument_spelling(name.getLocation()); written.isFileID()) {
            // One a body spells (`#define HAS_X defined(X)`) is the body's.
            add_unresolved(written);
        }
    }

    void MacroUndefined(const clang::Token& name,
                        const clang::MacroDefinition& md,
                        const clang::MacroDirective* undef) override {
        if(auto def = md.getMacroInfo()) {
            add_macro(def, MacroRef::Undef, name.getLocation());
        } else {
            add_unresolved(name.getLocation());
        }
    }

private:
    clang::FileID prev_fid;
    CompilationUnit& unit;

    /// The macro names recorded where an argument spells them.
    llvm::DenseSet<std::pair<const clang::MacroInfo*, clang::SourceLocation>> written_arguments;

    /// Each file's braces the macros it expanded so far open, net.
    llvm::DenseMap<clang::FileID, std::int32_t> macro_braces;
};

std::unique_ptr<clang::PPCallbacks> CompilationUnit::directive_recorder() {
    return std::make_unique<DirectiveRecorder>(*this);
}

}  // namespace clice
