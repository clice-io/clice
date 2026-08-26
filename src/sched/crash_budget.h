#pragma once

#include <chrono>

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// Content-keyed crash budget for shared build artifacts (PCH, PCM).
///
/// A document quarantine cannot contain a poison preamble or module
/// interface: the artifact is shared, so every dependent (or every session
/// with the same preamble) would re-trigger the build and kill workers of
/// its own. The budget is keyed by the artifact's content-derived cache key,
/// which makes recovery structural: editing the poison content changes the
/// key, and the fresh key starts with a fresh budget.
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

    /// Building the artifact with this key killed a worker.
    void on_crash(llvm::StringRef key) {
        auto& entry = crashes[key];
        entry.count += 1;
        entry.last_crash = std::chrono::steady_clock::now();
    }

    /// The artifact built: the key's strikes were transient, not poison —
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
