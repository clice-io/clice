#pragma once

#include <chrono>

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// Content-keyed strike budget for shared PCH pairs whose consumers keep
/// blaming them (see PCHFamily::blame).
///
/// The pair is shared, so one consumer's verdict cannot contain it: every
/// session with the same preamble would rebuild and blame it again. The
/// budget is keyed by the artifact's content-derived cache key, which makes
/// recovery structural: editing the content changes the key, and the fresh
/// key starts with a fresh budget.
class CrashBudget {
public:
    constexpr static unsigned threshold = 2;

    /// A block must never be final: the poison may live in content the key
    /// cannot see (a header included by the preamble text the pch_key
    /// hashes), so editing it cannot unlock the key. After the cooldown the
    /// key earns a fresh budget — the retry either succeeds or re-blocks
    /// after `threshold` more crashes. Mirrors slot revival: bounded burn,
    /// never a permanent verdict.
    explicit CrashBudget(std::chrono::steady_clock::duration retry_after =
                             std::chrono::minutes(5)) : retry_after(retry_after) {}

    /// The artifact with this key has spent its budget: refuse to build it.
    bool blocked(llvm::StringRef key) {
        auto it = crashes.find(key);
        if(it == crashes.end() || it->second.count < threshold) {
            return false;
        }
        if(std::chrono::steady_clock::now() - it->second.last_crash < retry_after) {
            return true;
        }
        // Cooldown over: a fresh budget, not a pardon (cf. revive_slot).
        crashes.erase(it);
        return false;
    }

    /// A consumer blamed the artifact with this key.
    void on_crash(llvm::StringRef key) {
        auto& entry = crashes[key];
        entry.count += 1;
        entry.last_crash = std::chrono::steady_clock::now();
    }

    /// A consumer used the artifact: the key's strikes were transient —
    /// without this, two unrelated hiccups far apart would block a key
    /// that rebuilds fine in between.
    void on_land(llvm::StringRef key) {
        crashes.erase(key);
    }

private:
    struct Entry {
        unsigned count = 0;
        std::chrono::steady_clock::time_point last_crash;
    };

    std::chrono::steady_clock::duration retry_after;

    /// Grows by one entry per crashing artifact key and shrinks as
    /// cooldowns expire — bounded by edits of poison content.
    llvm::StringMap<Entry> crashes;
};

}  // namespace clice
