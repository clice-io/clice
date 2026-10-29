module clice:server.document_failures;

import :feature.diagnostics;
import :server.session;
import :support.failure_record;
import :support.prelude;
import :vfs.file_table;

namespace clice {

/// What crashed on each document: one failure record per (document, request
/// kind), kept for the whole process beside the worker pool, so a document
/// keeps its records across a close and a move to another project. Every
/// transition goes through a method:
///
///  1. No crash loop — a kind that crashed stops until the document's
///     inputs change (an edit, a change to a file it reads, another
///     context) and the policy's spacing has passed, for at most
///     change_limit crashes in a row; past that only a save retries.
///     Nothing but a change, a save or the kind answering lets it run
///     again: a document that sits still is never retried.
///  2. One retry at a time — the attempt that retries a kind stops the
///     others until its outcome (Attempt); one that crashes on inputs
///     changed meanwhile leaves the change standing: the newer inputs are
///     still untried.
///  3. Crash accounting — crashed() is the only way failures count, the
///     request's fate aside; one worker death counts once however many of
///     the document's requests it failed, and only the kind answering on
///     the current buffer (succeeded) clears the record.
///  4. Visibility — a record shows in the document's diagnostics, but for
///     a first crash within the policy's editing window. A repeat shows.
class DocumentFailures {
public:
    using Clock = FailureRecord::Clock;

    /// The policy of every request kind.
    constexpr static FailurePolicy policy{
        .change_limit = 3,
        .spacing = std::chrono::seconds(2),
        .editing_window = std::chrono::seconds(10),
    };

    /// A dispatch of `kind` killed a worker on the document's content:
    /// `death` is the death's identity (worker::death_of), `cause` how it
    /// died. Returns whether the record shows.
    bool crashed(Fid document,
                 feature::RequestKind kind,
                 llvm::StringRef death,
                 std::string cause,
                 Clock::time_point now);

    /// A dispatch of `kind` answered on the buffer the ticket was taken on:
    /// while the buffer is still that one, the kind's record goes. An edit
    /// that arrived meanwhile keeps it: the new buffer is untried. Returns
    /// whether a record went.
    bool succeeded(const Ticket& ticket, feature::RequestKind kind);

    /// The document's inputs changed: every stopped kind may retry once the
    /// policy's spacing has passed since its crash.
    void changed(Fid document, Clock::time_point now);

    /// The user saved the document: every stopped kind retries at once,
    /// with a fresh count.
    void saved(Fid document);

    /// The document closed on a buffer hashing to `hash`.
    void closed(Fid document, std::uint64_t hash);

    /// The document opened on a buffer hashing to `hash`: closing it was no
    /// retry, but opening it on other bytes is a change.
    void opened(Fid document, std::uint64_t hash, Clock::time_point now);

    /// Whether `kind` crashed and may not retry now.
    bool stops(Fid document, feature::RequestKind kind, Clock::time_point now) const;

    /// The record of `kind`, stopping it or not; null for none.
    const FailureRecord* find(Fid document, feature::RequestKind kind) const;

    /// Whether the document holds a record of any kind.
    bool any(Fid document) const;

    /// The records the document's diagnostics show, in the order the kinds
    /// first crashed.
    llvm::SmallVector<feature::CrashNote> notes(Fid document) const;

    /// What a crash of the document's compile is put on while it is not
    /// known to be the document's own (see ASTFamily::attribute_crash).
    struct Suspects {
        /// A crash with clang-tidy on, held while the compile retries
        /// without the pass: the compile finishing charges it to the pass,
        /// another crash to the compile.
        std::optional<ipc::Error> tidy;

        /// The PCH pair a crash of the compile was put on: retracted, it is
        /// rebuilt, and a crash on it again is the document's own. Cleared
        /// by a compile that finishes.
        std::string pch;
    };

    Suspects& suspects(Fid document) {
        return documents[document].suspects;
    }

    /// A dispatch of a kind, held across its flight. One that holds the
    /// retry a change or a save allowed stops the kind's other dispatches
    /// until it ends, and gives the retry back when it ends without crashing
    /// or answering.
    class [[nodiscard]] Attempt {
    public:
        Attempt(DocumentFailures& failures, Fid document, feature::RequestKind kind);

        Attempt(const Attempt&) = delete;
        Attempt& operator=(const Attempt&) = delete;

        ~Attempt();

        /// Whether the kind crashed during this attempt's flight: it stops
        /// on the inputs this attempt carries, so they must not reach a
        /// worker again.
        bool crashed_meanwhile() const;

    private:
        DocumentFailures& failures;

        Fid document;

        feature::RequestKind kind;

        /// The document's change count and the kind's crash serial as the
        /// attempt started.
        std::uint64_t changes;

        std::uint64_t serial = 0;

        /// This attempt retries the kind (FailureRecord::trying).
        bool retrying = false;
    };

private:
    struct Record {
        FailureRecord failure;

        /// The serial of its last crash (see crashes).
        std::uint64_t serial = 0;
    };

    struct Document {
        /// Insertion-ordered, so notes keep a stable order across
        /// publishes.
        llvm::MapVector<feature::RequestKind, Record> records;

        Suspects suspects;

        Clock::time_point changed_at;

        /// The user saved the inputs as they are: nothing half-typed.
        bool saved_since_change = false;

        /// Bumped by every change; an Attempt tells by it whether the
        /// inputs moved during its flight.
        std::uint64_t changes = 0;

        /// The hash of the buffer the document closed on; none while open.
        std::optional<std::uint64_t> closed_on;
    };

    Record* find_record(Fid document, feature::RequestKind kind);

    /// Grows by one entry per document closed, edited or crashed.
    llvm::DenseMap<Fid, Document> documents;

    /// Bumped by every counted crash; an Attempt tells by it whether its
    /// kind crashed during its flight, which a count cannot — a save
    /// resets that.
    std::uint64_t crashes = 0;
};

}  // namespace clice
