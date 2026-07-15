#pragma once

#include <algorithm>
#include <chrono>
#include <string>

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// Per-document crash containment: the state machine behind quarantining a
/// document whose content keeps killing workers.
///
/// The pool's crash budget lives on slots; the poison lives in content. This
/// type owns the document half of that split, and every transition goes
/// through a method — the fields are private precisely so the invariants
/// cannot be broken from a call site:
///
///  1. Blame conservation — on_crash() is the only way evidence accrues, and
///     nothing but land() (inherited evidence only) or reset() removes it.
///     A crash recorded mid-request survives that request's success.
///  2. Bounded blast radius — blocked() flips at `threshold` crashes and
///     stays until a probe is armed; each real content change arms at most
///     one probe (on_edit), consumed by exactly one attempt (spend_probe),
///     returned only when the attempt provably never ran (re_arm_probe).
///  3. Visibility — a quarantine spell asks to be announced exactly once
///     (needs_announcement / mark_announced); leaving quarantine re-arms
///     the announcement for the next spell.
///
/// The Flight token pins invariant 1 structurally: a compile snapshots the
/// evidence it inherited at takeoff and can only clear that much on landing,
/// so a success cannot launder crashes that happened while it flew.
class Quarantine {
public:
    /// Consecutive worker kills a document gets before it is refused
    /// further dispatches. Two: one crash can be an unlucky coincidence
    /// (OOM racing the accounting), two in a row for the same content is
    /// a pattern.
    constexpr static unsigned threshold = 2;

    /// The evidence a compile inherited at takeoff; land() clears no more.
    class Flight {
        friend class Quarantine;
        unsigned inherited = 0;
        unsigned inherited_total = 0;
    };

    /// Crashes currently blamed on this document's content.
    unsigned crashes() const {
        return streak + query_streak;
    }

    /// The document has spent its crash budget: compile and query evidence
    /// pool into one budget — both are this content killing workers.
    bool active() const {
        return crashes() >= threshold;
    }

    /// Quarantined with no probe attempt armed: refuse dispatches.
    bool blocked() const {
        return active() && !probe;
    }

    /// A dispatch carrying this document's content killed a worker.
    /// `death` is the worker incarnation's identity (worker::death_of):
    /// one process death fails every request in flight on it, and a death
    /// already counted — by either ledger — is not counted again.
    void on_crash(llvm::StringRef death = {}) {
        if(!counted(death)) {
            streak += 1;
        }
    }

    /// Snapshot the inherited evidence at compile takeoff.
    Flight begin_flight() const {
        Flight flight;
        flight.inherited = streak;
        flight.inherited_total = crashes();
        return flight;
    }

    /// Evidence accrued since this flight took off (a PCH build inside its
    /// own dependency prep, a concurrent completion build of the same
    /// content). Used with active() to stop a request whose dependency
    /// phase already tipped the document into quarantine.
    bool grew(Flight flight) const {
        return crashes() > flight.inherited_total;
    }

    /// A full compile of the current content landed: the inherited compile
    /// evidence is disproved, but crashes recorded during the flight will
    /// recur on the next request, and query evidence is untouched — the
    /// compile succeeding says nothing about the queries that follow it.
    void land(Flight flight) {
        streak -= std::min(flight.inherited, streak);
        if(!active()) {
            announced_spell = false;
        }
    }

    /// A query on this document's settled AST killed a worker. A separate
    /// ledger from compile crashes: a successful compile does not disprove
    /// it — the same AST crashes the same query again — so only a query
    /// that answers (on_query_land) clears it. Without the split, the
    /// crash-triggered recompile would land, launder the evidence, and the
    /// next query would kill a worker again, forever.
    void on_query_crash(llvm::StringRef death = {}) {
        if(!counted(death)) {
            query_streak += 1;
        }
    }

    /// A query answered: queries on the current content provably do not
    /// kill workers.
    void on_query_land() {
        query_streak = 0;
        if(!active()) {
            announced_spell = false;
        }
    }

    /// A content change grants a quarantined document one probe attempt.
    /// It deliberately does NOT reset the streak: only a compile that
    /// succeeds proves the document healthy — resetting on edits would let
    /// a poison file under active editing crash a worker per keystroke and
    /// never reach quarantine. Dropped and no-op edits (`changed == false`)
    /// grant nothing: the poison bytes are unchanged.
    void on_edit(bool changed) {
        if(changed && active()) {
            probe = true;
        }
    }

    /// The probe attempt is being spent: at dispatch, or when the attempt's
    /// own dependency phase crashed (that WAS the attempt).
    void spend_probe() {
        probe = false;
    }

    /// The attempt provably never ran (no expendable worker to host it):
    /// keep the license so a later request retries.
    void re_arm_probe() {
        if(active()) {
            probe = true;
        }
    }

    /// True until the current quarantine spell has been announced to the
    /// client; a document must never go silently dead.
    bool needs_announcement() const {
        return active() && !announced_spell;
    }

    void mark_announced() {
        announced_spell = true;
    }

    /// The document was (re)opened: fresh content, fresh record.
    void reset() {
        streak = 0;
        query_streak = 0;
        probe = false;
        announced_spell = false;
        last_death.clear();
    }

private:
    /// Whether this death was already counted. Remembering only the most
    /// recent identity suffices: a peer teardown fails all of a death's
    /// in-flight requests in the same loop turn, so their errors arrive
    /// adjacently. Anonymous evidence (no identity) always counts.
    bool counted(llvm::StringRef death) {
        if(death.empty()) {
            return false;
        }
        if(death == last_death) {
            return true;
        }
        last_death = death.str();
        return false;
    }

    unsigned streak = 0;
    unsigned query_streak = 0;
    bool probe = false;
    bool announced_spell = false;
    std::string last_death;
};

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
    constexpr static unsigned threshold = Quarantine::threshold;

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
