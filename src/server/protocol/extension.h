#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kota/ipc/lsp/protocol.h"

namespace clice::ext {

struct ContextItem {
    std::string label;
    std::string description;

    /// Host source file (header contexts) or the file itself (source
    /// compile configurations).
    std::string uri;

    /// For header contexts: which include of the header in its direct
    /// includer this context represents (0-based, in directive order).
    /// Present only when the header is included more than once.
    std::optional<std::uint32_t> occurrence;

    /// For source compile configurations: canonical hash identifying the
    /// CDB entry. Pass it back in switchContext to select this entry.
    std::optional<std::string> command_hash;
};

struct QueryContextParams {
    std::string uri;
    std::optional<int> offset;
};

struct QueryContextResult {
    std::vector<ContextItem> contexts;
    int total = 0;

    /// Workspace state generation these results were computed against.
    /// Pass it back in switchContext to detect stale listings.
    std::uint64_t epoch = 0;
};

struct CurrentContextParams {
    std::string uri;
};

struct CurrentContextResult {
    std::optional<ContextItem> context;
};

struct SwitchContextParams {
    std::string uri;
    std::string context_uri;

    /// Include occurrence to pin (header contexts, 0-based).
    std::optional<std::uint32_t> occurrence;

    /// Canonical CDB entry hash to pin (source files with multiple
    /// compile commands).
    std::optional<std::string> command_hash;

    /// Epoch of the queryContext result this choice came from. When set
    /// and the workspace has changed since, the switch is rejected with
    /// stale = true and the client should re-query.
    std::optional<std::uint64_t> epoch;
};

struct SwitchContextResult {
    bool success = false;

    /// The request referenced an outdated queryContext listing.
    bool stale = false;
};

/// Pushed as the clice/inactiveRegions notification after each compile:
/// the preprocessor-inactive regions of the file under its current
/// compilation context. Clients typically render them dimmed.
struct InactiveRegionsParams {
    std::string uri;
    std::vector<kota::ipc::protocol::Range> regions;
};

/// clice/internal/poll — TEST-ONLY, not a stable API. Synchronously runs
/// one file-tracker tick (stat → diff → events → dispatch → effects) and
/// responds only once the effects are applied, so integration tests can
/// disable the polling loops and get "change disk → poll → assert"
/// determinism with zero sleeps. Absent from capabilities and user docs.
struct PollParams {
    /// Which loop to tick: "cdb" or "workspace".
    std::string loop;
};

struct PollResult {
    /// Number of file events the tick produced and dispatched.
    std::uint32_t events = 0;
};

/// Test hook (clice/internal/logFlood): emit `count` info-level log lines
/// of roughly `size` bytes each, tagged stderr-flood with a running index.
/// Gives backpressure tests a deterministic volume source — feature log
/// lines change shape over time and must not be load-bearing for tests.
/// Absent from capabilities and user docs.
struct LogFloodParams {
    std::uint32_t count = 0;
    std::uint32_t size = 0;
};

struct LogFloodResult {
    std::uint32_t emitted = 0;
};

}  // namespace clice::ext
