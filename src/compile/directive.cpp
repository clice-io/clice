module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :compile.directive;
import :syntax.lexer;
import :vfs.path;

namespace clice {

class CompilationUnit::DirectiveCollector : public clang::PPCallbacks {
public:
    DirectiveCollector(CompilationUnit& unit) : unit(unit) {}

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
        }
        bool taken = negated ? def == nullptr : def != nullptr;
        add_condition(loc, kind, taken ? Condition::True : Condition::False);
    }

    void add_macro(const clang::MacroInfo* def, MacroRef::Kind kind, clang::SourceLocation loc) {
        if(def->isBuiltinMacro()) {
            return;
        }

        /// FIXME: A macro defined in <built-in> or <command line> (`-D`) gets no
        /// definition record, so definition, references and symbol search on a
        /// `-D` macro find no symbol; hover still shows it.
        if(unit.is_builtin_file(unit.file_id(loc))) {
            return;
        }

        auto& directive = unit.collected_directives[unit.file_id(loc)];
        directive.macros.emplace_back(MacroRef{def, kind, loc});
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

    /// A lookup that found nothing still consulted the disk: a file created
    /// at any place it looked changes what the compile sees. Every such
    /// place — the directory the includer was opened from, for a quoted
    /// name, then each search directory the lookup walks — is recorded as
    /// an absent input, spelled as clang looked: `..` resolves past a
    /// symlinked directory.
    void add_absent(llvm::StringRef name, bool angled, clang::SourceLocation location) {
        auto& absent = unit.absent_paths;
        auto& files = unit.instance->getFileManager();
        auto add = [&](llvm::StringRef directory) {
            llvm::SmallString<256> candidate(directory);
            path::append(candidate, name);
            files.makeAbsolutePath(candidate);
            absent.insert(Spelling::absolute(candidate).str());
        };
        if(path::is_absolute(name)) {
            absent.insert(Spelling::absolute(name).str());
            return;
        }
        if(!angled) {
            if(auto includer = unit.SM().getFileEntryRefForID(unit.file_id(location))) {
                add(includer->getDir().getName());
            }
        }
        auto& search = unit.instance->getPreprocessor().getHeaderSearchInfo();
        for(auto it = angled ? search.angled_dir_begin() : search.quoted_dir_begin();
            it != search.search_dir_end();
            ++it) {
            // FIXME: header maps and framework directories have no plain directory:
            // a failed lookup through them records no absent input, so a header
            // created there later goes unnoticed.
            if(auto directory = it->getDirRef()) {
                add(directory->getName());
            }
        }
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
        });
    }

    void LexedFileChanged(clang::FileID curr_fid,
                          LexedFileChangeReason reason,
                          clang::SrcMgr::CharacteristicKind,
                          clang::FileID prev_fid,
                          clang::SourceLocation) override {
        if(reason == LexedFileChangeReason::EnterFile && curr_fid.isValid() && prev_fid.isValid() &&
           this->prev_fid.isValid() && prev_fid == this->prev_fid) {
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
            /// File with guard will have only one file id in `SourceManager`, use
            /// `translateFile` to find it.
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
            clang::SourceRange,
            clang::PPCallbacks::ConditionValueKind value) override {
        add_condition(loc, Condition::If, value);
    }

    void Elif(clang::SourceLocation loc,
              clang::SourceRange,
              clang::PPCallbacks::ConditionValueKind value,
              clang::SourceLocation) override {
        /// FIXME: The macros of an `#elif` past a taken branch are never
        /// evaluated, so they get no reference.
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
    void Elifdef(clang::SourceLocation loc, clang::SourceRange, clang::SourceLocation) override {
        /// FIXME: The macro a skipped `#elifdef` / `#elifndef` names gets no
        /// reference, so macro references miss these sites.
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
    void Elifndef(clang::SourceLocation loc, clang::SourceRange, clang::SourceLocation) override {
        add_condition(loc, Condition::Elifndef, Condition::Skipped);
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
        if(auto def = definition.getMacroInfo()) {
            add_macro(def, MacroRef::Ref, name.getLocation());
        }
    }

    void Defined(const clang::Token& name,
                 const clang::MacroDefinition& definition,
                 clang::SourceRange) override {
        if(auto def = definition.getMacroInfo()) {
            add_macro(def, MacroRef::Ref, name.getLocation());
        }
    }

    void MacroUndefined(const clang::Token& name,
                        const clang::MacroDefinition& md,
                        const clang::MacroDirective* undef) override {
        if(auto def = md.getMacroInfo()) {
            add_macro(def, MacroRef::Undef, name.getLocation());
        }
    }

private:
    clang::FileID prev_fid;
    CompilationUnit& unit;
};

std::unique_ptr<clang::PPCallbacks> CompilationUnit::directive_collector() {
    return std::make_unique<DirectiveCollector>(*this);
}

}  // namespace clice
