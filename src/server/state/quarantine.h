#pragma once

#include <algorithm>

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
    };

    /// Crashes currently blamed on this document's content.
    unsigned crashes() const {
        return streak;
    }

    /// The document has spent its crash budget.
    bool active() const {
        return streak >= threshold;
    }

    /// Quarantined with no probe attempt armed: refuse dispatches.
    bool blocked() const {
        return active() && !probe;
    }

    /// A dispatch carrying this document's content killed a worker.
    void on_crash() {
        streak += 1;
    }

    /// Snapshot the inherited evidence at compile takeoff.
    Flight begin_flight() const {
        Flight flight;
        flight.inherited = streak;
        return flight;
    }

    /// Evidence accrued since this flight took off (a PCH build inside its
    /// own dependency prep, a concurrent completion build of the same
    /// content). Used with active() to stop a request whose dependency
    /// phase already tipped the document into quarantine.
    bool grew(Flight flight) const {
        return streak > flight.inherited;
    }

    /// A full compile of the current content landed: the inherited evidence
    /// is disproved, but crashes recorded during the flight will recur on
    /// the next request and must keep accumulating toward quarantine.
    void land(Flight flight) {
        streak -= std::min(flight.inherited, streak);
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
        probe = false;
        announced_spell = false;
    }

private:
    unsigned streak = 0;
    bool probe = false;
    bool announced_spell = false;
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

    /// The artifact with this key has spent its budget: refuse to build it.
    bool blocked(llvm::StringRef key) const {
        auto it = crashes.find(key);
        return it != crashes.end() && it->second >= threshold;
    }

    /// Building the artifact with this key killed a worker.
    void on_crash(llvm::StringRef key) {
        crashes[key] += 1;
    }

private:
    /// Grows by one entry per crashing artifact key — bounded by edits of
    /// poison content, so no eviction is needed.
    llvm::StringMap<unsigned> crashes;
};

}  // namespace clice
