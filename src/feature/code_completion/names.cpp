module clice;

import :feature.code_completion.names;
import :support.fuzzy_matcher;

namespace clice::feature {

/// Whether `name` may be one of the matches of `lower`, the typed name
/// lowercased: its characters in `name` in order, the first on what may
/// begin a word — the start, past a separator, an uppercase letter. The
/// matcher's cost would add up over every identifier of a large preamble.
static bool may_match(llvm::StringRef lower, llvm::StringRef name) {
    std::size_t next = 0;
    for(std::size_t i = 0; i < name.size() && next < lower.size(); i += 1) {
        char c = name[i];
        if(llvm::toLower(c) != lower[next]) {
            continue;
        }
        if(next == 0 && i != 0 && llvm::isAlnum(lower[0]) && llvm::isAlnum(name[i - 1]) &&
           !llvm::isUpper(c)) {
            continue;
        }
        next += 1;
    }
    return next == lower.size();
}

TypedNames::TypedNames(clang::Sema& sema,
                       clang::ExternalSemaSource& reader,
                       clang::IdentifierInfoLookup& identifiers,
                       llvm::StringRef typed,
                       std::size_t limit) :
    sema(sema), reader(reader), identifier_reader(identifiers), typed(typed), limit(limit) {}

llvm::IntrusiveRefCntPtr<TypedNames> TypedNames::open(clang::Sema& sema,
                                                      llvm::StringRef typed,
                                                      std::size_t limit) {
    auto& ast = sema.getASTContext();
    auto& pp = sema.getPreprocessor();
    auto* reader = llvm::dyn_cast_or_null<clang::ExternalSemaSource>(ast.getExternalSource());
    auto* macro_reader = pp.getExternalSource();
    auto* identifier_reader = ast.Idents.getExternalIdentifierLookup();
    if(!reader || !macro_reader || !identifier_reader) {
        return nullptr;
    }
    llvm::IntrusiveRefCntPtr<TypedNames> names(
        new TypedNames(sema, *reader, *identifier_reader, typed, limit));
    names->AddSource(reader);
    names->macros.emplace(*names, *macro_reader);
    ast.setExternalSource(names);
    pp.setExternalSource(&*names->macros);
    if(!sema.getLangOpts().CPlusPlus) {
        names->identifiers.emplace(*names, *identifier_reader);
        ast.Idents.setExternalIdentifierLookup(&*names->identifiers);
    }
    return names;
}

llvm::ArrayRef<llvm::StringRef> TypedNames::matching() {
    if(matched) {
        return *matched;
    }
    auto lower = llvm::StringRef(typed).lower();
    // A name reserved to the implementation is the library's own, wanted
    // only when typed so.
    bool reserved_typed = llvm::StringRef(typed).starts_with("_");
    FuzzyMatcher matcher(typed);
    std::vector<std::pair<float, llvm::StringRef>> scored;
    llvm::StringSet<> seen;
    std::unique_ptr<clang::IdentifierIterator> names(identifier_reader.getIdentifiers());
    for(auto name = names->Next(); !name.empty(); name = names->Next()) {
        if(!reserved_typed && name.starts_with("_") &&
           (name.starts_with("__") || (name.size() > 1 && llvm::isUpper(name[1])))) {
            continue;
        }
        if(!may_match(lower, name)) {
            continue;
        }
        if(auto score = matcher.match(name); score && seen.insert(name).second) {
            scored.emplace_back(*score, name);
        }
    }
    ranges::stable_sort(scored,
                        [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    matched.emplace();
    matched->reserve(scored.size());
    for(auto& [score, name]: scored) {
        matched->push_back(name);
    }
    return *matched;
}

std::vector<llvm::StringRef>
    TypedNames::read_matching(llvm::function_ref<bool(llvm::StringRef)> names_something) {
    // Most identifiers name nothing in one scope: members, parameters, names
    // of other namespaces. Those tried are bounded too.
    std::vector<llvm::StringRef> found;
    for(auto name: matching()) {
        if(found_total == limit || tried_total == limit * 40) {
            cut = true;
            break;
        }
        tried_total += 1;
        if(names_something(name)) {
            found.push_back(name);
            found_total += 1;
        }
    }
    return found;
}

void TypedNames::completeVisibleDeclsMap(const clang::DeclContext* context) {
    if(!context->isFileContext()) {
        MultiplexExternalSemaSource::completeVisibleDeclsMap(context);
        return;
    }
    auto* space = const_cast<clang::DeclContext*>(context)->getPrimaryContext();
    if(!read.insert(space).second) {
        return;
    }
    auto& ast = sema.getASTContext();
    auto& language = sema.getLangOpts();
    read_matching([&](llvm::StringRef name) {
        auto* identifier = &ast.Idents.get(name);
        return !identifier->isKeyword(language) && !space->lookup(identifier).empty();
    });
    // An operator's name is no identifier.
    if(FuzzyMatcher(typed).match("operator")) {
        for(int kind = clang::OO_None + 1; kind < clang::NUM_OVERLOADED_OPERATORS; kind += 1) {
            space->lookup(ast.DeclarationNames.getCXXOperatorName(
                static_cast<clang::OverloadedOperatorKind>(kind)));
        }
    }
}

void TypedNames::updateOutOfDateIdentifier(const clang::IdentifierInfo& identifier) {
    reader.updateOutOfDateIdentifier(identifier);
}

void TypedNames::Macros::ReadDefinedMacros() {
    // An identifier comes with the macro the loaded files define by its
    // name.
    auto& pp = names.sema.getPreprocessor();
    names.read_matching(
        [&](llvm::StringRef name) { return pp.getIdentifierInfo(name)->hasMacroDefinition(); });
}

clang::IdentifierIterator* TypedNames::Identifiers::getIdentifiers() {
    class Found final : public clang::IdentifierIterator {
    public:
        explicit Found(std::vector<llvm::StringRef> names) : names(std::move(names)) {}

        llvm::StringRef Next() override {
            if(next == names.size()) {
                return {};
            }
            next += 1;
            return names[next - 1];
        }

    private:
        std::vector<llvm::StringRef> names;

        std::size_t next = 0;
    };
    // An identifier comes with the declarations the loaded files make of it
    // in the translation unit; its macro the macros' own read counts.
    auto& sema = names.sema;
    return new Found(names.read_matching([&](llvm::StringRef name) {
        auto* identifier = &sema.getASTContext().Idents.get(name);
        return sema.IdResolver.begin(identifier) != sema.IdResolver.end();
    }));
}

}  // namespace clice::feature
