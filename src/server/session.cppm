module clice:server.session;

import :feature.diagnostics;
import :support.failure_record;
import :support.prelude;
import :syntax.position;
import :syntax.source_text;
import :vfs.file_table;

namespace clice {

/// One document open in the editor: its buffer, the generation every request
/// checks it by, and what crashed on it. Opened by didOpen, gone at didClose;
/// a move to another project keeps it.
///
/// Open files are never depended upon by other files: dependencies always
/// point to disk files, and a buffer shadows the disk for its own compile
/// only.
///
/// The crash records hold one failure record per request kind that crashed
/// a worker on the document's content, and live as long as the document is
/// open. Every transition goes through a method:
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
class Session {
public:
    using Clock = FailureRecord::Clock;

    /// The policy of every request kind.
    constexpr static FailurePolicy policy{
        .change_limit = 3,
        .spacing = std::chrono::seconds(2),
        .editing_window = std::chrono::seconds(10),
    };

    Session(Fid path_id, std::string text, int version);

    llvm::StringRef text() const {
        return source->text();
    }

    PositionMap positions() const {
        return source->positions();
    }

    /// Replace the text, and with it everything derived from it.
    void set_text(std::string text);

    /// Apply a didChange: fold the content changes into the buffer (ranges
    /// in the client's `encoding`), then take `version` and bump the
    /// generation. A range that does not fit the buffer is clamped to it, as
    /// LSP 3.17 asks, and the client's drift is logged: dropping the edit
    /// would desync every later position. A change of the text is a change
    /// of the document's inputs (changed). Returns whether the text changed:
    /// an edit can replace text with the same text.
    bool edit(llvm::ArrayRef<protocol::TextDocumentContentChangeEvent> changes,
              int version,
              PositionEncoding encoding);

    /// A dispatch of `kind` killed a worker on the document's content:
    /// `death` is the death's identity (worker::death_of), `cause` how it
    /// died. Returns whether the record shows.
    bool crashed(feature::RequestKind kind,
                 llvm::StringRef death,
                 std::string cause,
                 Clock::time_point now);

    /// A dispatch of `kind` answered on the buffer of `generation`: while
    /// the buffer is still that one, the kind's record goes. An edit that
    /// arrived meanwhile keeps it: the new buffer is untried. Returns
    /// whether a record went.
    bool succeeded(feature::RequestKind kind, std::uint64_t generation);

    /// The document's inputs changed: every stopped kind may retry once the
    /// policy's spacing has passed since its crash.
    void changed(Clock::time_point now);

    /// The user saved the document: every stopped kind retries at once,
    /// with a fresh count.
    void saved();

    /// Whether `kind` crashed and may not retry now.
    bool crash_stops(feature::RequestKind kind, Clock::time_point now) const;

    /// The record of `kind`, stopping it or not; null for none.
    const FailureRecord* crash_record(feature::RequestKind kind) const;

    /// The records the document's diagnostics show, in the order the kinds
    /// first crashed.
    llvm::SmallVector<feature::CrashNote> crash_notes() const;

    /// A dispatch of a kind, held across its flight. One that holds the
    /// retry a change or a save allowed stops the kind's other dispatches
    /// until it ends, and gives the retry back when it ends without crashing
    /// or answering. The session outlives it: the request holding it holds
    /// the session.
    class [[nodiscard]] Attempt {
    public:
        Attempt(Session& session, feature::RequestKind kind);

        Attempt(const Attempt&) = delete;
        Attempt& operator=(const Attempt&) = delete;

        ~Attempt();

        /// Whether the kind crashed during this attempt's flight: it stops
        /// on the inputs this attempt carries, so they must not reach a
        /// worker again.
        bool crashed_meanwhile() const;

    private:
        Session& session;

        feature::RequestKind kind;

        /// The document's change count and the kind's crash sequence number
        /// as the attempt started.
        std::uint64_t changes;

        std::uint64_t seq = 0;

        /// This attempt retries the kind (FailureRecord::trying).
        bool retrying = false;
    };

    /// What a crash of the document's compile is put on while it is not
    /// known to be the document's own (see ServedProject::attribute_crash).
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

    Fid path_id;

    /// LSP document version, incremented by the client on each edit.
    int version = 0;

    /// Current buffer content (may differ from disk until saved) and what
    /// is derived from it, read in the dialect the server lexes it in.
    /// Behind a pointer: a SourceText stays where it was made, its text
    /// pointing into itself, and a compile's output keeps the text it
    /// compiled.
    std::shared_ptr<SourceText> source;

    /// xxh3 of the text, rewritten with it: what "the buffer holds these
    /// bytes" is compared by against disk observations and index shards.
    std::uint64_t hash = 0;

    /// Bumped by every edit, a move to another project, another context and
    /// the close: whatever a request computed under an older one describes
    /// a buffer, a project or a context that no longer serves the document.
    std::uint64_t generation = 0;

    /// The document closed: nothing of it is published any more.
    bool closed = false;

    Suspects suspects;

private:
    struct Record {
        FailureRecord failure;

        /// The sequence number of its last crash (see crashes).
        std::uint64_t seq = 0;
    };

    Record* find_record(feature::RequestKind kind);

private:
    /// Insertion-ordered, so notes keep a stable order across publishes.
    llvm::MapVector<feature::RequestKind, Record> records;

    Clock::time_point changed_at;

    /// The user saved the inputs as they are: nothing half-typed.
    bool saved_since_change = false;

    /// Bumped by every change; an Attempt tells by it whether the inputs
    /// moved during its flight.
    std::uint64_t changes = 0;

    /// Bumped by every counted crash; an Attempt tells by it whether its
    /// kind crashed during its flight, which a count cannot — a save
    /// resets that.
    std::uint64_t crashes = 0;
};

/// A request's hold on the buffer it was asked about: the generation
/// snapshot taken as the request is dispatched.
/// Every later decision — adopting a compile product, committing a worker
/// reply, answering at all — asks `fresh()` first; an edit, a move or the
/// close bumped the generation, and whatever the request computed describes
/// a buffer that no longer exists. Completion still answers after edits at
/// or past its cursor: its ranges still hold, and the client filters it by
/// what was typed meanwhile.
struct Ticket {
    std::shared_ptr<Session> session;
    std::uint64_t generation = 0;

    /// An empty ticket for no session: a document not open.
    static Ticket take(std::shared_ptr<Session> session) {
        auto generation = session ? session->generation : 0;
        return {std::move(session), generation};
    }

    bool fresh() const {
        return session->generation == generation;
    }
};

}  // namespace clice
