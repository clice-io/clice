module clice;

import :compile.compilation;

namespace clice {

static std::uint64_t mix(std::uint64_t hash, std::uint64_t value) {
    return (hash ^ value) * 0x100000001b3ULL + (hash >> 29);
}

/// What a file entry hands the parser follows from its text, the branches
/// its conditions take, the files its includes find and each macro expanded
/// in it, nested ones too, by its definition: hashed in the order the
/// preprocessor meets them, rather than token by token.
class CompilationUnit::IdentityRecorder : public clang::PPCallbacks {
public:
    explicit IdentityRecorder(CompilationUnit& unit) :
        unit(unit), sm(unit.SM()), identities(unit.collected_identities) {
        auto& lang = unit.instance->getLangOpts();
        base = llvm::xxh3_64bits(unit.instance->getTarget().getTriple().str());
        base = mix(base, static_cast<std::uint64_t>(lang.LangStd));
        for(bool flag: {static_cast<bool>(lang.CPlusPlus),
                        static_cast<bool>(lang.GNUMode),
                        static_cast<bool>(lang.MSVCCompat),
                        static_cast<bool>(lang.CharIsSigned),
                        static_cast<bool>(lang.Exceptions),
                        static_cast<bool>(lang.CXXExceptions),
                        static_cast<bool>(lang.RTTI),
                        static_cast<bool>(lang.Char8)}) {
            base = mix(base, flag ? 3 : 5);
        }
    }

    /// Counts the tokens of the files whose declaration context is not
    /// taken yet.
    void token(const clang::Token& token) {
        if(pending.empty() || token.isAnnotation() ||
           token.isOneOf(clang::tok::eod, clang::tok::eof)) {
            return;
        }
        auto fid = sm.getFileID(sm.getExpansionLoc(token.getLocation()));
        if(auto it = pending.find(fid); it != pending.end()) {
            it->second += 1;
            if(it->second == 2) {
                add(fid, unit.declaration_context());
                pending.erase(it);
            }
        }
    }

private:
    void FileChanged(clang::SourceLocation location,
                     FileChangeReason reason,
                     clang::SrcMgr::CharacteristicKind kind,
                     clang::FileID previous) override {
        if(reason == ExitFile) {
            // A file of fewer than two tokens: its context as it leaves.
            if(auto it = pending.find(previous); it != pending.end()) {
                add(previous, unit.declaration_context());
                pending.erase(it);
            }
            return;
        }
        if(reason != EnterFile) {
            return;
        }
        auto fid = sm.getFileID(location);
        identities[fid] = mix(mix(base, static_cast<std::uint64_t>(kind)), content_key(fid));
        // The parser reads a token ahead: an include right after a
        // namespace's `}` is entered before the parser leaves the namespace.
        // The context is taken once the file's first token is consumed, as
        // its second is read.
        pending[fid] = 0;
    }

    void If(clang::SourceLocation location, clang::SourceRange, ConditionValueKind value) override {
        condition(location, 1, value);
    }

    void Elif(clang::SourceLocation location,
              clang::SourceRange,
              ConditionValueKind value,
              clang::SourceLocation) override {
        condition(location, 2, value);
    }

    void Ifdef(clang::SourceLocation location,
               const clang::Token&,
               const clang::MacroDefinition& definition) override {
        condition(location, 3, definition ? 1 : 0);
    }

    void Ifndef(clang::SourceLocation location,
                const clang::Token&,
                const clang::MacroDefinition& definition) override {
        condition(location, 4, definition ? 1 : 0);
    }

    void Elifdef(clang::SourceLocation location,
                 const clang::Token&,
                 const clang::MacroDefinition& definition) override {
        condition(location, 5, definition ? 1 : 0);
    }

    void Elifdef(clang::SourceLocation location,
                 clang::SourceRange,
                 clang::SourceLocation) override {
        condition(location, 5, 2);
    }

    void Elifndef(clang::SourceLocation location,
                  const clang::Token&,
                  const clang::MacroDefinition& definition) override {
        condition(location, 6, definition ? 1 : 0);
    }

    void Elifndef(clang::SourceLocation location,
                  clang::SourceRange,
                  clang::SourceLocation) override {
        condition(location, 6, 2);
    }

    void Else(clang::SourceLocation location, clang::SourceLocation) override {
        condition(location, 7, 0);
    }

    void Endif(clang::SourceLocation location, clang::SourceLocation) override {
        condition(location, 8, 0);
    }

    void InclusionDirective(clang::SourceLocation hash,
                            const clang::Token&,
                            llvm::StringRef,
                            bool,
                            clang::CharSourceRange,
                            clang::OptionalFileEntryRef file,
                            llvm::StringRef,
                            llvm::StringRef,
                            const clang::Module*,
                            bool,
                            clang::SrcMgr::CharacteristicKind) override {
        auto [fid, offset] = sm.getDecomposedExpansionLoc(hash);
        add(fid, mix(offset, file ? path_key(*file) : 0));
    }

    void MacroExpands(const clang::Token& name,
                      const clang::MacroDefinition& definition,
                      clang::SourceRange,
                      const clang::MacroArgs*) override {
        auto* macro = definition.getMacroInfo();
        if(!macro) {
            return;
        }
        auto [fid, offset] = sm.getDecomposedExpansionLoc(name.getLocation());
        if(macro->isBuiltinMacro() && unit_specific(*name.getIdentifierInfo())) {
            unit_only.insert(fid);
            identities[fid] = 0;
            return;
        }
        add(fid, mix(offset, unit.macro_state(macro, *name.getIdentifierInfo())));
    }

    void add(clang::FileID fid, std::uint64_t value) {
        if(unit_only.contains(fid)) {
            return;
        }
        auto& identity = identities[fid];
        identity = mix(identity, value);
    }

    void condition(clang::SourceLocation location, std::uint64_t kind, std::uint64_t value) {
        auto [fid, offset] = sm.getDecomposedExpansionLoc(location);
        add(fid, mix(kind * 7 + value, offset));
    }

    /// The builtin macros whose value is this unit's, not the file's.
    static bool unit_specific(const clang::IdentifierInfo& name) {
        auto spelling = name.getName();
        return spelling == "__COUNTER__" || spelling == "__INCLUDE_LEVEL__" ||
               spelling == "__BASE_FILE__";
    }

    std::uint64_t path_key(clang::FileEntryRef file) {
        return llvm::xxh3_64bits(unit.file_path(file));
    }

    /// A file's text, the same in every unit while the file is unchanged.
    std::uint64_t content_key(clang::FileID fid) {
        auto entry = sm.getFileEntryRefForID(fid);
        if(!entry) {
            return 1;
        }
        auto [it, fresh] = content_keys.try_emplace(&entry->getFileEntry(), 0);
        if(fresh) {
            if(auto buffer = sm.getBufferOrNone(fid)) {
                it->second = mix(llvm::xxh3_64bits(buffer->getBuffer()), path_key(*entry));
            }
        }
        return it->second;
    }

private:
    CompilationUnit& unit;

    clang::SourceManager& sm;

    llvm::DenseMap<clang::FileID, std::uint64_t>& identities;

    /// The language and the target.
    std::uint64_t base = 0;

    /// The entries whose declaration context is not taken yet, with how
    /// many of their tokens the parser read.
    llvm::DenseMap<clang::FileID, std::uint32_t> pending;

    /// The entries a unit_specific builtin expands in: their identity stays
    /// 0.
    llvm::DenseSet<clang::FileID> unit_only;

    llvm::DenseMap<const clang::FileEntry*, std::uint64_t> content_keys;
};

std::unique_ptr<clang::PPCallbacks> CompilationUnit::identity_recorder() {
    assert(params.kind == CompilationKind::Whole ||
           params.kind == CompilationKind::PrecompiledHeader);
    auto recorder = std::make_unique<IdentityRecorder>(*this);
    watch_tokens([watch = recorder.get()](const clang::Token& token) { watch->token(token); });
    return recorder;
}

std::uint64_t CompilationUnit::macro_state(const clang::MacroInfo* macro,
                                           const clang::IdentifierInfo& name) {
    if(!macro) {
        return 0;
    }
    auto [it, fresh] = macro_states.try_emplace(macro, 0);
    if(!fresh) {
        return it->second;
    }
    auto& state = it->second;
    if(macro->isBuiltinMacro()) {
        state = llvm::xxh3_64bits(name.getName());
        return state;
    }
    // A definition read from a PCH keeps no pointer to its literals' text,
    // which its buffer, in the PCH too, still has.
    auto& pp = instance->getPreprocessor();
    llvm::SmallString<32> buffer;
    state = mix(macro->isFunctionLike() ? 7 : 11, macro->getNumParams());
    for(const auto& token: macro->tokens()) {
        state = mix(state, static_cast<std::uint64_t>(token.getKind()));
        if(auto* identifier = token.getIdentifierInfo()) {
            state = mix(state, llvm::xxh3_64bits(identifier->getName()));
        } else if(token.isLiteral()) {
            state = mix(state, llvm::xxh3_64bits(pp.getSpelling(token, buffer)));
        }
    }
    auto& sm = SM();
    auto [fid, offset] = sm.getDecomposedLoc(sm.getFileLoc(macro->getDefinitionLoc()));
    if(auto entry = sm.getFileEntryRefForID(fid)) {
        state = mix(state, mix(llvm::xxh3_64bits(file_path(*entry)), offset));
    }
    return state;
}

std::uint64_t CompilationUnit::declaration_context() {
    auto& sema = instance->getSema();
    std::uint64_t context = 0;
    for(auto* dc = sema.CurContext; dc; dc = dc->getParent()) {
        context = mix(context, static_cast<std::uint64_t>(dc->getDeclKind()));
        if(auto* named = llvm::dyn_cast<clang::NamedDecl>(dc)) {
            context = mix(context, llvm::xxh3_64bits(named->getNameAsString()));
        } else if(auto* linkage = llvm::dyn_cast<clang::LinkageSpecDecl>(dc)) {
            context = mix(context, static_cast<std::uint64_t>(linkage->getLanguage()));
        }
    }
    return mix(context, sema.PragmaAttributeStack.size());
}

std::uint64_t CompilationUnit::record_packing() {
    return clang::Sema::AlignPackInfo::getRawEncoding(
        instance->getSema().AlignPackStack.CurrentValue);
}

}  // namespace clice
