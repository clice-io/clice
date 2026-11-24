/// What a compile's completion makes of each of Sema's replies: the items
/// it offers, matched against the name typed, bundled and ranked.

module clice:feature.code_completion.reply;

import :compile.compilation;
import :feature.code_completion.cursor;
import :feature.code_completion;
import :semantic.display;
import :support.fuzzy_matcher;
import :support.prelude;
import :syntax.position;
import :syntax.token;

namespace clice::feature {

/// An item of the reply and the declaration whose comment documents it,
/// read once the compile is done.
struct Item {
    protocol::CompletionItem item;

    const clang::NamedDecl* documented = nullptr;
};

/// One completion a compile answers, as each of Sema's replies reads it:
/// the compile and the point, what is asked, what the replies tell back,
/// and what printing their items shares. A nested run in the whole
/// buffer's compile is a completion of its own.
struct Completing {
    CompilationUnit& unit;

    /// The text completed in: the focus file's, but in a nested run in the
    /// whole buffer's compile, whose own reads a name at the point.
    llvm::StringRef text;

    std::uint32_t offset;

    PositionEncoding encoding;

    const CodeCompletionOptions& options;

    const CompletionClient& client;

    const CompileCompletion& names;

    /// In a nested run, the type the completion's own context expects, by
    /// its hash (EntityTable::type_hash): the whole buffer's compile has no
    /// parse at the point to tell it.
    std::uint64_t expected_type = 0;

    /// Set once an item needs the header of its symbol included (Completion).
    bool& includes_pending;

    /// Set when the reply holds fewer names than there are: the list's
    /// limit cut it, or the index or the loaded files have more of a scope
    /// for the name typed.
    bool& more_names;

    clang::CodeCompletionTUInfo info{std::make_shared<clang::GlobalCodeCompletionAllocator>()};

    std::optional<display::ReadableTypes> readable;

    display::Options display_options = {
        .suppress_unwritten_scope = true,
        .clean_uglified = true,
    };
};

/// One candidate as the item it becomes, before matching and bundling.
struct Candidate {
    std::string label;
    protocol::CompletionItemKind kind;

    /// What accepting inserts; the label when empty.
    std::string insert;
    bool snippet = false;

    /// What the typed prefix is matched against; the label when empty.
    std::string filter;

    /// Overload bundle the candidate joins; none when empty.
    std::string bundle;

    /// What accepting inserts when the candidate's bundle holds more than
    /// it: the call without placeholders. Empty when the candidate
    /// inserts no call.
    std::string bundled_insert;
    bool bundled_snippet = false;

    std::string signature;
    std::string return_type;
    bool deprecated = false;
    bool macro = false;

    /// The declaration whose comment documents the candidate.
    const clang::NamedDecl* documented = nullptr;

    /// Multiplies the score of the name's match: how strongly the context
    /// asks for the candidate.
    float relevance = 1.0F;

    /// Edits elsewhere in the text accepting also makes: the `.` of a
    /// member access through a pointer turned into `->`, an `import`.
    std::vector<TextReplacement> edits;

    /// The symbol whose header accepting must include (ScopeCandidate::symbol).
    std::uint64_t include = 0;

    /// Breaks ties in score: a field's place in a designated initializer.
    std::optional<std::uint32_t> order;
};

/// The label details of an item standing for `count` overloads of a name.
protocol::CompletionItemLabelDetails bundle_details(std::size_t count);

/// The items one of Sema's replies offers (CodeCompleteConsumer::
/// ProcessCodeCompleteResults), added phase by phase — Sema's candidates,
/// the override declarations, the names of other namespaces, the `import`
/// of an implementation unit — then finished.
class Reply {
public:
    /// The reply to `context`; none where the name at the point maps to no
    /// position.
    static auto open(Completing& completing, clang::CodeCompletionContext& context)
        -> std::optional<Reply>;

    void add_candidates(llvm::MutableArrayRef<clang::CodeCompletionResult> candidates);

    /// Where a member declaration starts, as Sema offers its override
    /// declarations.
    void add_overrides();

    /// The names of other namespaces the prefix matches, with the qualifier
    /// they need, or with the `import` or the `#include` the file needs to
    /// reach them; after a qualifier naming a namespace, those of it the
    /// file does not reach yet, the name alone; in the reply of a nested
    /// run, `nested`, only after a qualifier.
    void add_other_namespaces(bool nested);

    /// SemaCodeComplete's `import name;` pattern skips implementation
    /// units, which import past their module declaration just the same.
    void add_import();

    /// The items: the declarations the list keeps rendered, the overloads
    /// of a name bundled, the items of one label deduplicated.
    auto finish() -> std::vector<Item>;

private:
    /// What the text after the name starts with.
    enum class Follows : std::uint8_t {
        Nothing,
        Parenthesis,
        Angle,
        Colon,
    };

    /// In `{ .` the fields rank in declaration order, the order C++
    /// designates them in: there those before the last one written
    /// cannot follow. A field designated as a whole cannot be again; one
    /// designated in part (`.inner.x = 1`) can (`.inner.y = 2`).
    struct Designators {
        llvm::StringMap<std::uint32_t> order;

        llvm::StringSet<> designated;
    };

    /// An item, with what decides which of the items of its label stays
    /// (dedup_priority).
    struct Listed {
        protocol::CompletionItem item;
        int priority = 0;
        const clang::NamedDecl* documented = nullptr;
    };

    /// The overloads of one name, as the item of the first one stands for
    /// them.
    struct Bundle {
        protocol::CompletionItem item;
        float score = 0.0F;
        std::uint32_t count = 0;
        int priority = 0;
        std::string bundled_insert;
        bool bundled_snippet = false;
        const clang::NamedDecl* documented = nullptr;
    };

    /// A declaration matched, rendered once all are known if the list
    /// keeps it (code_completion.limit).
    struct Pending {
        clang::CodeCompletionResult candidate;
        Candidate item;
        float score;
    };

    using Edit = std::variant<protocol::TextEdit, protocol::InsertReplaceEdit>;

    Reply(Completing& completing, clang::CodeCompletionContext& context, Prefix prefix, Edit edit);

    bool member_access() const;

    /// Whether the completion names something unqualified, where the names
    /// of other namespaces can be offered with the qualifier they need. What
    /// the system headers declare ranks after the rest where it competes
    /// with it: not among the members of a class or a scope.
    bool unqualified() const;

    /// Whether the context expects a function itself rather than the result
    /// of calling it: a function pointer or reference, a member pointer.
    bool expects_function() const;

    /// Call or template arguments already written after the name: a `<`
    /// opens a template's, and compares anything else.
    bool arguments_follow(bool templated) const;

    bool allowed(llvm::StringRef keyword) const;

    /// After `t.`, `t->` or `T::` on a template parameter, Sema offers
    /// what the constraints on it require as patterns: a name and, for
    /// a call, the types of the arguments it is called with.
    bool offers_requirements() const;

    std::optional<float> match(llvm::StringRef filter);

    protocol::CompletionItem item_of(const Candidate& candidate, float score);

    void add_item(const Candidate& candidate);

    void add_pattern(const clang::CodeCompletionResult& candidate);

    void add_keyword_pattern(const clang::CodeCompletionString& pattern, unsigned priority);

    /// Sema offers an Objective-C method as a pattern where it is written
    /// whole: its declaration in an implementation, after the `-` or the
    /// return type, its body left to write; after a dot one taking no
    /// argument, read as a property; and in a message to `super` the
    /// method being written, passed its arguments — in place of the
    /// method's own candidate.
    void add_method_pattern(const clang::CodeCompletionString& pattern,
                            const clang::ObjCMethodDecl& method,
                            unsigned priority);

    void add_requirement(const clang::CodeCompletionString& pattern, unsigned priority);

    void add_macro(clang::CodeCompletionResult& candidate);

    bool left_out(const clang::CodeCompletionResult& candidate,
                  const clang::CXXRecordDecl* naming) const;

    /// `relevance` scales what the context makes of the candidate.
    void add_declaration(clang::CodeCompletionResult& candidate, float relevance);

    /// A declaration's item past its name: what costs most, its
    /// signature and types, spent on those the list keeps alone.
    void render(clang::CodeCompletionResult& candidate, Candidate item);

    /// What accepting a call inserts after the name, unless the
    /// arguments are written already or the name is not called here:
    /// its placeholders, or its parentheses with the cursor between them
    /// — what a bundle of several overloads takes too. A call without
    /// parameters is complete with its parentheses.
    void insert_call(Candidate& item, const clang::CodeCompletionString& ccs, bool called_here);

    /// Whether `decl` is, names, or a call of it returns, the type the
    /// context expects.
    bool expected(const clang::NamedDecl* decl) const;

    /// The declarations the list keeps: those of the best `limit` labels,
    /// a bundle of overloads one, as completion_list ranks them.
    void render_kept();

private:
    Completing& completing;

    clang::Sema& sema;

    clang::CodeCompletionContext& context;

    Prefix prefix;

    FuzzyMatcher matcher;

    /// What every item replaces, its text left to each.
    Edit edit;

    Follows follows;

    /// In a module unit, imports precede every other declaration of its
    /// purview, none in its global module fragment, and only an interface
    /// exports; elsewhere `export` only begins one, before any declaration.
    /// Clang's keywords know neither.
    bool imports;

    bool exports;

    Designators designators;

    std::vector<Listed> items;

    llvm::MapVector<std::string, Bundle, llvm::StringMap<unsigned>> bundles;

    std::vector<Pending> pending;

    /// The declarations offered, canonical: the names of other namespaces
    /// leave them out.
    llvm::DenseSet<const clang::Decl*> offered;

    llvm::StringSet<> plain_patterns;

    /// The selectors offered to declare: Sema offers one for every
    /// method declaring it, its overrides among them.
    llvm::StringSet<> declared_selectors;
};

}  // namespace clice::feature
