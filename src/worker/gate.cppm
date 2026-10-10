module;

#include "modules/prelude.h"

module clice:worker.gate;

namespace clice {

/// The worker's side of the test gates (BuildProbe::gate). The master names
/// a request by its crash tag right before it sends the request; the work of
/// the first request of that tag to start parks where it starts, on the
/// thread that runs it, until the master releases the gate. The worker is
/// then busy with the request as with real work, so a cancel, an edit or a
/// death lands while the work is under way.
void install_test_gates(kota::ipc::BincodePeer& peer, kota::event_loop& loop);

/// Park the calling thread while a gate stands for `tag` (see CrashScope).
void park_at_test_gate(llvm::StringRef tag);

}  // namespace clice
