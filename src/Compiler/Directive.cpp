#include "Compiler/Directive.h"

#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"

namespace clice {

namespace {

class DirectiveCollector : public clang::PPCallbacks {
public:
    DirectiveCollector(clang::Preprocessor& pp,
                       llvm::DenseMap<clang::FileID, Directive>& directives) :
        pp(pp), sm(pp.getSourceManager()), directives(directives) {}

private:
    void add_condition(clang::SourceLocation location,
                       Condition::BranchKind kind,
                       Condition::ConditionValue value,
                       clang::SourceRange cond_range) {
        auto& directive = directives[sm.getFileID(location)];
        directive.conditions.emplace_back(kind, value, location, cond_range);
    }

    void add_condition(clang::SourceLocation loc,
                       Condition::BranchKind kind,
                       clang::PPCallbacks::ConditionValueKind value,
                       clang::SourceRange condition_range) {
        Condition::ConditionValue cond_value =
            value == clang::PPCallbacks::CVK_False          ? Condition::None
            : value == clang::PPCallbacks::CVK_True         ? Condition::True
            : value == clang::PPCallbacks::CVK_NotEvaluated ? Condition::Skipped
                                                            : Condition::None;
        add_condition(loc, kind, cond_value, condition_range);
    }

    void add_condition(clang::SourceLocation loc,
                       Condition::BranchKind kind,
                       const clang::Token& name,
                       const clang::MacroDefinition& definition) {
        if(auto def = definition.getMacroInfo()) {
            add_macro(def, MacroRef::Ref, name.getLocation());
            add_condition(loc, kind, Condition::True, name.getLocation());
        } else {
            add_condition(loc, kind, Condition::False, name.getLocation());
        }
    }

    void add_macro(const clang::MacroInfo* def, MacroRef::Kind kind, clang::SourceLocation loc) {
        if(def->isBuiltinMacro()) {
            return;
        }

        if(sm.isWrittenInBuiltinFile(loc) || sm.isWrittenInCommandLineFile(loc) ||
           sm.isWrittenInScratchSpace(loc)) {
            return;
        }

        auto& directive = directives[sm.getFileID(loc)];
        directive.macros.emplace_back(MacroRef{def, kind, loc});
    }

public:
    /// ============================================================================
    ///                         Rewritten Preprocessor Callbacks
    /// ============================================================================

    void InclusionDirective(clang::SourceLocation hash_loc,
                            const clang::Token& include_tok,
                            llvm::StringRef,
                            bool,
                            clang::CharSourceRange filename_range,
                            clang::OptionalFileEntryRef,
                            llvm::StringRef,
                            llvm::StringRef,
                            const clang::Module*,
                            bool,
                            clang::SrcMgr::CharacteristicKind) override {
        prev_fid = sm.getFileID(hash_loc);

        /// An `IncludeDirective` call is always followed by either a `LexedFileChanged`
        /// or a `FileSkipped`. so we cannot get the file id of included file here.
        directives[prev_fid].includes.emplace_back(Include{
            .fid = {},
            .location = include_tok.getLocation(),
            .filename_range = filename_range.getAsRange(),
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
            auto& include = directives[prev_fid].includes.back();
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
            auto& include = directives[prev_fid].includes.back();
            include.skipped = true;

            /// Get the FileID for the given file. If the source file is included multiple
            /// times, the FileID will be the first inclusion.
            include.fid = sm.translateFile(file);
        }
    }

    void moduleImport(clang::SourceLocation import_location,
                      clang::ModuleIdPath names,
                      const clang::Module*) override {
        auto fid = sm.getFileID(sm.getExpansionLoc(import_location));
        auto& import = directives[fid].imports.emplace_back();
        import.location = import_location;
        for(auto name: names) {
            import.name += name.getIdentifierInfo()->getName();
            import.name_locations.emplace_back(name.getLoc());
        }
    }

    void HasInclude(clang::SourceLocation location,
                    llvm::StringRef,
                    bool,
                    clang::OptionalFileEntryRef file,
                    clang::SrcMgr::CharacteristicKind) override {
        clang::FileID fid;
        if(file) {
            fid = sm.translateFile(*file);
        }

        directives[sm.getFileID(location)].has_includes.emplace_back(fid, location);
    }

    void PragmaDirective(clang::SourceLocation loc,
                         clang::PragmaIntroducerKind introducer) override {
        // Ignore other cases except starts with `#pragma`.
        if(introducer != clang::PragmaIntroducerKind::PIK_HashPragma)
            return;

        clang::FileID fid = sm.getFileID(loc);

        llvm::StringRef text_to_end = sm.getBufferData(fid).substr(sm.getFileOffset(loc));
        llvm::StringRef that_line = text_to_end.take_until([](char ch) { return ch == '\n'; });

        Pragma::Kind kind = that_line.contains("endregion") ? Pragma::EndRegion
                            : that_line.contains("region")  ? Pragma::Region
                                                            : Pragma::Other;

        auto& directive = directives[fid];
        directive.pragmas.emplace_back(Pragma{
            that_line,
            kind,
            loc,
        });
    }

    void If(clang::SourceLocation loc,
            clang::SourceRange cond_range,
            clang::PPCallbacks::ConditionValueKind value) override {
        add_condition(loc, Condition::If, value, cond_range);
    }

    void Elif(clang::SourceLocation loc,
              clang::SourceRange cond_range,
              clang::PPCallbacks::ConditionValueKind value,
              clang::SourceLocation) override {
        add_condition(loc, Condition::Elif, value, cond_range);
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
                 clang::SourceRange cond_range,
                 clang::SourceLocation) override {
        /// FIXME: should we try to evaluate the condition to compute the macro reference?
        add_condition(loc, Condition::Elifdef, Condition::Skipped, cond_range);
    }

    /// Invoke when #ifndef is taken.
    void Ifndef(clang::SourceLocation loc,
                const clang::Token& name,
                const clang::MacroDefinition& definition) override {
        add_condition(loc, Condition::Ifndef, name, definition);
    }

    // Invoke when #elifndef is taken.
    void Elifndef(clang::SourceLocation loc,
                  const clang::Token& name,
                  const clang::MacroDefinition& definition) override {
        add_condition(loc, Condition::Elifndef, name, definition);
    }

    // Invoke when #elifndef is skipped.
    void Elifndef(clang::SourceLocation loc,
                  clang::SourceRange cond_range,
                  clang::SourceLocation) override {
        add_condition(loc, Condition::Elifndef, Condition::Skipped, cond_range);
    }

    void Else(clang::SourceLocation loc, clang::SourceLocation if_loc) override {
        add_condition(loc, Condition::Else, Condition::None, clang::SourceRange());
    }

    void Endif(clang::SourceLocation loc, clang::SourceLocation if_loc) override {
        add_condition(loc, Condition::EndIf, Condition::None, clang::SourceRange());
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

    void MacroUndefined(const clang::Token& name,
                        const clang::MacroDefinition& md,
                        const clang::MacroDirective* undef) override {
        if(auto def = md.getMacroInfo()) {
            add_macro(def, MacroRef::Undef, name.getLocation());
        }
    }

private:
    clang::FileID prev_fid;
    clang::Preprocessor& pp;
    clang::SourceManager& sm;
    llvm::DenseMap<clang::FileID, Directive>& directives;
    llvm::DenseMap<clang::MacroInfo*, std::size_t> macro_cache;
};

}  // namespace

void Directive::attach(clang::Preprocessor& pp,
                       llvm::DenseMap<clang::FileID, Directive>& directives) {
    pp.addPPCallbacks(std::make_unique<DirectiveCollector>(pp, directives));
}

}  // namespace clice
