#include "Compiler/Directive.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/Preprocessor.h"

namespace clice {

namespace {

class DirectiveCollector : public clang::PPCallbacks {
public:
    DirectiveCollector(clang::Preprocessor& PP,
                       llvm::DenseMap<clang::FileID, Directive>& directives) :
        PP(PP), SM(PP.getSourceManager()), directives(directives) {}

private:
    void add_condition(clang::SourceLocation location,
                       Condition::BranchKind kind,
                       Condition::ConditionValue value,
                       clang::SourceRange cond_range) {
        auto& directive = directives[SM.getFileID(location)];
        directive.conditions.emplace_back(kind, value, location, cond_range);
    }

    void add_condition(clang::SourceLocation loc,
                       Condition::BranchKind kind,
                       clang::PPCallbacks::ConditionValueKind value,
                       clang::SourceRange conditionRange) {
        Condition::ConditionValue cond_value =
            value == clang::PPCallbacks::CVK_False          ? Condition::None
            : value == clang::PPCallbacks::CVK_True         ? Condition::True
            : value == clang::PPCallbacks::CVK_NotEvaluated ? Condition::Skipped
                                                            : Condition::None;
        add_condition(loc, kind, cond_value, conditionRange);
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

        if(SM.isWrittenInBuiltinFile(loc) || SM.isWrittenInCommandLineFile(loc) ||
           SM.isWrittenInScratchSpace(loc)) {
            return;
        }

        auto& directive = directives[SM.getFileID(loc)];
        directive.macros.emplace_back(MacroRef{def, kind, loc});
    }

public:
    /// ============================================================================
    ///                         Rewritten Preprocessor Callbacks
    /// ============================================================================

    void InclusionDirective(clang::SourceLocation hashLoc,
                            const clang::Token& includeTok,
                            llvm::StringRef,
                            bool,
                            clang::CharSourceRange filenameRange,
                            clang::OptionalFileEntryRef,
                            llvm::StringRef,
                            llvm::StringRef,
                            const clang::Module*,
                            bool,
                            clang::SrcMgr::CharacteristicKind) override {
        prevFID = SM.getFileID(hashLoc);

        /// An `IncludeDirective` call is always followed by either a `LexedFileChanged`
        /// or a `FileSkipped`. so we cannot get the file id of included file here.
        directives[prevFID].includes.emplace_back(Include{
            .fid = {},
            .location = includeTok.getLocation(),
            .filename_range = filenameRange.getAsRange(),
        });
    }

    void LexedFileChanged(clang::FileID currFID,
                          LexedFileChangeReason reason,
                          clang::SrcMgr::CharacteristicKind,
                          clang::FileID prevFID,
                          clang::SourceLocation) override {
        if(reason == LexedFileChangeReason::EnterFile && currFID.isValid() && prevFID.isValid() &&
           this->prevFID.isValid() && prevFID == this->prevFID) {
            /// Once the file has changed, it means that the last include is not skipped.
            /// Therefore, we initialize its file id with the current file id.
            auto& include = directives[prevFID].includes.back();
            include.skipped = false;
            include.fid = currFID;
        }
    }

    void FileSkipped(const clang::FileEntryRef& file,
                     const clang::Token&,
                     clang::SrcMgr::CharacteristicKind) override {
        if(prevFID.isValid()) {
            /// File with guard will have only one file id in `SourceManager`, use
            /// `translateFile` to find it.
            auto& include = directives[prevFID].includes.back();
            include.skipped = true;

            /// Get the FileID for the given file. If the source file is included multiple
            /// times, the FileID will be the first inclusion.
            include.fid = SM.translateFile(file);
        }
    }

    void moduleImport(clang::SourceLocation import_location,
                      clang::ModuleIdPath names,
                      const clang::Module*) override {
        auto fid = SM.getFileID(SM.getExpansionLoc(import_location));
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
            fid = SM.translateFile(*file);
        }

        directives[SM.getFileID(location)].has_includes.emplace_back(fid, location);
    }

    void PragmaDirective(clang::SourceLocation Loc,
                         clang::PragmaIntroducerKind Introducer) override {
        // Ignore other cases except starts with `#pragma`.
        if(Introducer != clang::PragmaIntroducerKind::PIK_HashPragma)
            return;

        clang::FileID fid = SM.getFileID(Loc);

        llvm::StringRef textToEnd = SM.getBufferData(fid).substr(SM.getFileOffset(Loc));
        llvm::StringRef thatLine = textToEnd.take_until([](char ch) { return ch == '\n'; });

        Pragma::Kind kind = thatLine.contains("endregion") ? Pragma::EndRegion
                            : thatLine.contains("region")  ? Pragma::Region
                                                           : Pragma::Other;

        auto& directive = directives[fid];
        directive.pragmas.emplace_back(Pragma{
            thatLine,
            kind,
            Loc,
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

    void Else(clang::SourceLocation loc, clang::SourceLocation ifLoc) override {
        add_condition(loc, Condition::Else, Condition::None, clang::SourceRange());
    }

    void Endif(clang::SourceLocation loc, clang::SourceLocation ifLoc) override {
        add_condition(loc, Condition::EndIf, Condition::None, clang::SourceRange());
    }

    void MacroDefined(const clang::Token& name, const clang::MacroDirective* MD) override {
        if(auto def = MD->getMacroInfo()) {
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
                        const clang::MacroDefinition& MD,
                        const clang::MacroDirective* undef) override {
        if(auto def = MD.getMacroInfo()) {
            add_macro(def, MacroRef::Undef, name.getLocation());
        }
    }

private:
    clang::FileID prevFID;
    clang::Preprocessor& PP;
    clang::SourceManager& SM;
    llvm::DenseMap<clang::FileID, Directive>& directives;
    llvm::DenseMap<clang::MacroInfo*, std::size_t> macroCache;
};

}  // namespace

void Directive::attach(clang::Preprocessor& pp,
                       llvm::DenseMap<clang::FileID, Directive>& directives) {
    pp.addPPCallbacks(std::make_unique<DirectiveCollector>(pp, directives));
}

}  // namespace clice
