#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// Per-document crash containment: what a document's worker crashes bar it
/// from, and when it may try again.
///
/// Records are kept per kind of work (a caller-defined discriminator: the
/// compile, the PCH build, each query kind), so a crashing hover bars hover
/// alone. Every transition goes through a method — the fields are private
/// precisely so the invariants cannot be broken from a call site:
///
///  1. No crash loop — a kind that crashed is barred until its inputs
///     change (an edit to the file, a change to a file it includes) and
///     retry_spacing has passed, at most max_strikes crashes in a row; past
///     that only a save retries. Nothing but a change, a save or the kind
///     answering lifts a bar: a document that sits still is never retried.
///  2. One attempt per license — the attempt that uses a retry license
///     spends it (Attempt), so concurrent dispatches wait for its outcome;
///     an attempt that ends without crashing or answering hands it back,
///     and one that crashes on inputs changed meanwhile leaves the change
///     standing: the newer inputs are still untried.
///  3. Blame conservation — on_crash() is the only way strikes accrue; one
///     worker death counts once however many of the document's requests it
///     failed, and only the kind answering (on_land) or a save clears them.
///  4. Visibility — a record shows in the document's diagnostics, except a
///     first crash on inputs changed within editing_window and not saved
///     since: half-typed code crashing is the common case there, and the
///     next edit usually fixes it. A repeat shows.
///
/// The record outlives the session: a reopened document is still barred.
class Quarantine {
public:
    using Clock = std::chrono::steady_clock;

    /// Crashes in a row after which only a save retries.
    constexpr static unsigned max_strikes = 3;

    /// Least time between a crash and the automatic retry a change earns:
    /// keystrokes through a half-typed construct must not spend the
    /// strikes.
    constexpr static std::chrono::seconds retry_spacing{2};

    /// A first crash on inputs changed this recently stays silent.
    constexpr static std::chrono::seconds editing_window{10};

    /// A dispatch of `kind` killed a worker. `death` is the worker
    /// incarnation's identity (worker::death_of): a death already counted
    /// is not counted again. `cause` is how it died, for the note.
    void on_crash(std::uint8_t kind,
                  llvm::StringRef death,
                  std::string cause,
                  Clock::time_point now);

    /// A dispatch of `kind` answered: its record goes.
    void on_land(std::uint8_t kind);

    /// The document's inputs changed: every barred kind may retry once
    /// retry_spacing has passed since its crash.
    void on_change(Clock::time_point now);

    /// The user saved the document: every barred kind retries at once,
    /// with a fresh run of strikes.
    void on_save();

    /// Whether `kind` crashed and has no license to try again.
    bool barred(std::uint8_t kind, Clock::time_point now) const;

    /// Whether `kind` has a record at all — barred or holding a license.
    bool crashed(std::uint8_t kind) const {
        return records.contains(kind);
    }

    /// A dispatch of `kind`, held across its flight. Under a record's
    /// license it spends the license and hands it back at destruction
    /// unless a crash or the kind answering settled the record meanwhile
    /// (invariant 2). Construct only for an admitted dispatch (!barred).
    class [[nodiscard]] Attempt {
    public:
        Attempt(Quarantine& quarantine, std::uint8_t kind);

        Attempt(const Attempt&) = delete;
        Attempt& operator=(const Attempt&) = delete;

        ~Attempt();

        /// Whether the kind crashed during this attempt's flight: its
        /// inputs are barred, so it must not dispatch again.
        bool overtaken() const;

    private:
        Quarantine& quarantine;
        std::uint8_t kind;
        std::uint64_t changes;
        std::uint64_t crash = 0;
        bool saved = false;
        bool changed = false;
    };

    /// Whether the record of `kind` shows in the diagnostics.
    bool shows(std::uint8_t kind) const {
        auto it = records.find(kind);
        return it != records.end() && it->second.visible;
    }

    /// A record the diagnostics show.
    struct Note {
        std::uint8_t kind;
        llvm::StringRef cause;
        unsigned strikes;
        /// Only a save retries: the strikes ran out.
        bool save_only;
    };

    llvm::SmallVector<Note> notes() const;

    bool empty() const {
        return records.empty();
    }

private:
    struct Record {
        unsigned strikes = 0;
        /// The serial of its last crash (see crashes).
        std::uint64_t crash = 0;
        std::string cause;
        Clock::time_point last_crash;
        /// The inputs changed since the last crash.
        bool changed = false;
        /// A save granted an immediate retry.
        bool saved = false;
        bool visible = false;
    };

    /// Whether this death was already counted. Remembering only the most
    /// recent identity suffices: a peer teardown fails all of a death's
    /// in-flight requests in the same loop turn, so their errors arrive
    /// adjacently. Anonymous evidence (no identity) always counts.
    bool counted(llvm::StringRef death);

    /// Insertion-ordered, so notes keep a stable order across publishes.
    llvm::MapVector<std::uint8_t, Record> records;
    Clock::time_point last_change;
    /// The user saved the inputs as they are: nothing half-typed.
    bool saved_since_change = false;
    /// Bumped by every on_change; an Attempt tells by it whether the inputs
    /// moved during its flight.
    std::uint64_t changes = 0;
    /// Bumped by every counted crash; an Attempt tells by it whether its
    /// kind crashed during its flight, which a strike count cannot — a save
    /// resets that.
    std::uint64_t crashes = 0;
    std::string last_death;
};

}  // namespace clice
