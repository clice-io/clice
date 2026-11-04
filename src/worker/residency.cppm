module clice:worker.residency;

import :support.prelude;

namespace clice {

/// Which open documents' ASTs the stateful workers keep, and which to
/// evict: the pool's one table of where each document's AST lives. It
/// decides, the pool carries the decisions out (an Evict to the worker),
/// and these are its only writers. It keeps:
///
/// - at most `limit` documents with an AST, but for the held ones: a
///   document is held while a request about it is in flight or a request
///   holds it, and is never evicted while held;
/// - a document's entry from its assignment to a worker until it is
///   evicted, forgotten or its worker dies; a compile reply counts only for the
///   assignment it was sent under, as the document may have been forgotten
///   and assigned anew since;
/// - the holds apart from the entries: a worker's death removes its
///   entries and leaves the holds, so a request resent after it still holds its
///   document.
class Residency {
public:
    explicit Residency(std::size_t limit) : limit(limit) {}

    struct Assignment {
        std::size_t worker = 0;

        /// Unique among the assignments of every document.
        std::uint64_t id = 0;
    };

    /// A document's AST a worker is to drop.
    struct Eviction {
        std::size_t worker = 0;

        /// The document as the worker names it.
        std::string path;
    };

    /// The document's assignment, if it has one.
    std::optional<Assignment> assignment(std::uint32_t document) const;

    /// Assign the document, which has no assignment, to `worker`; `path` is
    /// how the worker names it.
    Assignment assign(std::uint32_t document, std::size_t worker, llvm::StringRef path);

    /// The documents assigned to the worker.
    std::size_t documents_on(std::size_t worker) const;

    /// A request about the document arrived or went to its worker: it is
    /// the most recently used. One with no assignment has no place to keep.
    void used(std::uint32_t document);

    /// A compile of the document sent under `assignment` replied: `done`,
    /// the worker holds the AST it built. One not done may have ended
    /// before it dropped the AST of the compile before, which no query
    /// reaches any more but the limit counts until a compile is done or
    /// the document is evicted.
    void compiled(std::uint32_t document, std::uint64_t assignment, bool done);

    /// Whether a worker holds the AST of the document's last compile.
    bool resident(std::uint32_t document) const;

    void hold(std::uint32_t document);
    void release(std::uint32_t document);

    /// The worker died: its documents have no assignment and no AST.
    /// Returns them.
    llvm::SmallVector<std::uint32_t> worker_died(std::size_t worker);

    /// The document closed or moved to another project: it has no
    /// assignment, and its worker drops what it holds of it, if any.
    std::optional<Eviction> forget(std::uint32_t document);

    /// While more documents have an AST than the limit allows, the least
    /// recently used one nobody holds loses its assignment: the evictions
    /// to carry out.
    llvm::SmallVector<Eviction> evictions();

private:
    struct Entry {
        Assignment assignment;

        std::string path;

        /// The worker may hold an AST of it: the limit counts it.
        bool ast = false;

        /// The worker holds the AST of its last compile: queries go there.
        bool answers = false;

        /// When a request about it was last sent, on clock's scale.
        std::uint64_t last_use = 0;
    };

    std::size_t limit;

    llvm::DenseMap<std::uint32_t, Entry> entries;

    /// The holds on each held document.
    llvm::DenseMap<std::uint32_t, unsigned> holds;

    std::uint64_t next_assignment = 1;

    std::uint64_t clock = 0;
};

}  // namespace clice
