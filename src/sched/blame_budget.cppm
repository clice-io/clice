module;

#include "modules/prelude.h"

module clice:sched.blame_budget;

namespace clice {

/// Content-keyed budget for shared PCH pairs whose consumers keep blaming
/// them (see PCHFamily::blame).
///
/// The pair is shared, so one consumer's verdict cannot contain it: every
/// session with the same preamble would rebuild and blame it again. The
/// budget is keyed by the artifact's content-derived cache key, which makes
/// recovery structural: editing the content changes the key, and the fresh
/// key starts with a fresh budget.
class BlameBudget {
public:
    constexpr static unsigned threshold = 2;

    /// A block must never be final: the poison may live in content the key
    /// cannot see (a header included by the preamble text the pch_key
    /// hashes), so editing it cannot unlock the key. After the cooldown the
    /// key earns a fresh budget — the retry either succeeds or re-blocks
    /// after `threshold` more blames. Mirrors slot revival: bounded burn,
    /// never a permanent verdict.
    explicit BlameBudget(std::chrono::steady_clock::duration retry_after =
                             std::chrono::minutes(5)) : retry_after(retry_after) {}

    /// The artifact with this key has spent its budget: refuse to build it.
    bool blocked(llvm::StringRef key) {
        auto it = blames.find(key);
        if(it == blames.end() || it->second.count < threshold) {
            return false;
        }
        if(std::chrono::steady_clock::now() - it->second.last_blame < retry_after) {
            return true;
        }
        // Cooldown over: a fresh budget, not a pardon (cf. revive_slot).
        blames.erase(it);
        return false;
    }

    /// A consumer blamed the artifact with this key.
    void on_blame(llvm::StringRef key) {
        auto& entry = blames[key];
        entry.count += 1;
        entry.last_blame = std::chrono::steady_clock::now();
    }

    /// A consumer used the artifact: the key's strikes were transient —
    /// without this, two unrelated hiccups far apart would block a key
    /// that rebuilds fine in between.
    void on_land(llvm::StringRef key) {
        blames.erase(key);
    }

private:
    struct Entry {
        unsigned count = 0;
        std::chrono::steady_clock::time_point last_blame;
    };

    std::chrono::steady_clock::duration retry_after;

    /// Grows by one entry per blamed key and shrinks as cooldowns expire or
    /// consumers land.
    llvm::StringMap<Entry> blames;
};

}  // namespace clice
