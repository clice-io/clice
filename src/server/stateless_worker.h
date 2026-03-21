#pragma once

namespace clice {

/// Run the stateless worker process mode.
/// The worker receives one-shot compilation tasks (BuildPCH, BuildPCM,
/// Completion, SignatureHelp, Index) via stdin/stdout bincode IPC,
/// executes them on a thread pool, and returns results.
int run_stateless_worker_mode();

}  // namespace clice
