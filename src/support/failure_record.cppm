module clice:support.failure_record;

import :support.prelude;

namespace clice {

/// When an operation that failed may run again. Each operation has its
/// own: a document's requests, a PCH's build and its consumption, a
/// module's build, a toolchain query.
struct FailurePolicy {
    using Clock = std::chrono::steady_clock;

    /// Failures in a row that stop the operation.
    unsigned limit = 1;

    /// Failures in a row from which a change of the inputs no longer lets
    /// the operation retry, only a save does; zero for no such bound.
    unsigned change_limit = 0;

    /// Least time between a failure and the retry a change of the inputs
    /// allows: keystrokes through a half-typed construct must not spend
    /// the failures.
    Clock::duration spacing{};

    /// How long the operation stays stopped after its last failure, if
    /// nothing else lets it retry sooner. A failure past it counts from one
    /// again.
    Clock::duration cooldown = Clock::duration::max();

    /// A first failure on inputs changed this recently, and not saved
    /// since, does not show: half-typed code crashing is the common case
    /// there, and the next edit usually fixes it.
    Clock::duration editing_window{};
};

/// What is remembered of an operation that failed: how many times in a
/// row, when and why it last failed, and what lets it run again. Only a
/// success of the same operation clears it: its owner forgets the record.
struct FailureRecord {
    using Clock = FailurePolicy::Clock;

    /// The operation failed again. `death` is the identity of the worker
    /// death it failed with (worker::death_of), empty for a failure no
    /// worker died of: a death fails every request in flight on its worker
    /// and counts once, so one counted already is not counted again.
    /// `editing`: the inputs changed within the policy's editing window and
    /// were not saved since. Returns whether the failure counted.
    bool fail(const FailurePolicy& policy,
              llvm::StringRef death,
              std::string cause,
              Clock::time_point now,
              bool editing = false);

    /// Whether the record stops the operation now: an attempt retrying it
    /// stops the others.
    bool stops(const FailurePolicy& policy, Clock::time_point now) const;

    /// A save lets the operation retry at once, with a fresh count.
    void save() {
        count = 0;
        saved = true;
    }

    /// Whether a worker died of it: every death has an identity.
    bool crashed() const {
        return !death.empty();
    }

    /// Whether the operation retries only on a save: a change of its
    /// inputs no longer lets it.
    bool save_only(const FailurePolicy& policy) const {
        return policy.change_limit != 0 && count >= policy.change_limit;
    }

    /// Failures in a row.
    unsigned count = 0;

    Clock::time_point failed_at;

    /// How it last failed, as the user reads it.
    std::string cause;

    /// The identity of the last worker death counted.
    std::string death;

    /// The inputs changed since the last failure.
    bool changed = false;

    /// A save lets the operation retry at once.
    bool saved = false;

    /// An attempt is retrying the operation: the others wait for its
    /// outcome instead of retrying beside it.
    bool trying = false;

    /// The record shows in the diagnostics of the document it stops.
    bool shown = false;
};

}  // namespace clice
