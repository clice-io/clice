module;

/// The names a completion's compile reads of the files it loads — a
/// precompiled preamble, the modules it imports — where a name is typed.

module clice:feature.code_completion.names;

import :support.prelude;

namespace clice::feature {

/// Of the files the compile loads, the names Sema lists where a name is
/// typed: as it reads a namespace, the macros or a C translation unit's
/// names, which live on their identifiers, the identifiers the typed name
/// matches are read there, best first, until `limit` of them name
/// something in the scopes read so far; the other names stay unread. Sema
/// decides which scopes it reads, and reads everything else whole.
class TypedNames final : public clang::MultiplexExternalSemaSource {
public:
    /// Taking over the sources of `sema`'s AST, preprocessor and identifier
    /// table; none where the compile loads no AST file.
    static llvm::IntrusiveRefCntPtr<TypedNames> open(clang::Sema& sema,
                                                     llvm::StringRef typed,
                                                     std::size_t limit);

    void completeVisibleDeclsMap(const clang::DeclContext* context) override;

    /// MultiplexExternalSemaSource leaves it out: an identifier a feature
    /// brings up to date itself (display::type's `bool`) asks the AST's
    /// source.
    void updateOutOfDateIdentifier(const clang::IdentifierInfo& identifier) override;

    /// Whether a scope Sema read holds more matching names than were read.
    bool more() const {
        return cut;
    }

private:
    /// The macros as Sema lists them.
    class Macros final : public clang::ExternalPreprocessorSource {
    public:
        Macros(TypedNames& names, clang::ExternalPreprocessorSource& reader) :
            names(names), reader(reader) {}

        void ReadDefinedMacros() override;

        void updateOutOfDateIdentifier(const clang::IdentifierInfo& identifier) override {
            reader.updateOutOfDateIdentifier(identifier);
        }

        clang::IdentifierInfo* GetIdentifier(std::uint64_t id) override {
            return reader.GetIdentifier(id);
        }

        clang::Module* getModule(unsigned id) override {
            return reader.getModule(id);
        }

    private:
        TypedNames& names;

        clang::ExternalPreprocessorSource& reader;
    };

    /// The identifiers as Sema lists a C translation unit's names.
    class Identifiers final : public clang::IdentifierInfoLookup {
    public:
        Identifiers(TypedNames& names, clang::IdentifierInfoLookup& reader) :
            names(names), reader(reader) {}

        clang::IdentifierInfo* get(llvm::StringRef name) override {
            return reader.get(name);
        }

        clang::IdentifierIterator* getIdentifiers() override;

    private:
        TypedNames& names;

        clang::IdentifierInfoLookup& reader;
    };

    TypedNames(clang::Sema& sema,
               clang::ExternalSemaSource& reader,
               clang::IdentifierInfoLookup& identifiers,
               llvm::StringRef typed,
               std::size_t limit);

    /// The identifiers of the loaded files `typed` matches, best first,
    /// each once; read from their identifier tables at the first ask.
    llvm::ArrayRef<llvm::StringRef> matching();

    /// Reads the matching identifiers in turn until `limit` of them name
    /// something, by `names_something`; the names that do.
    std::vector<llvm::StringRef>
        read_matching(llvm::function_ref<bool(llvm::StringRef)> names_something);

private:
    clang::Sema& sema;

    clang::ExternalSemaSource& reader;

    clang::IdentifierInfoLookup& identifier_reader;

    std::string typed;

    std::size_t limit;

    std::optional<Macros> macros;

    std::optional<Identifiers> identifiers;

    std::optional<std::vector<llvm::StringRef>> matched;

    /// The namespaces read.
    llvm::DenseSet<const clang::DeclContext*> read;

    /// The names read, of every scope: a list shows `limit` items at most,
    /// the innermost scope's first, as Sema reads it first.
    std::size_t found_total = 0;

    /// The identifiers tried, of every scope.
    std::size_t tried_total = 0;

    bool cut = false;
};

}  // namespace clice::feature
