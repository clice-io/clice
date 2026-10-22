module;

#include "modules/prelude.h"

module clice:server.live_sources;

import :index.query;
import :project.project;
import :sched.families.pch;
import :server.document_compile;
import :server.session_store;

namespace clice {

/// The server's live index sources: the open buffers' own file indexes
/// (current per the AST family's latest compiles), the PCH overlays their preambles
/// were compiled into, and the preamble regions those overlays indexed.
class ServerLiveSources final : public index::LiveSources {
public:
    ServerLiveSources(Project& project,
                      PCHFamily& pch,
                      const SessionStore& sessions,
                      const DocumentCompiles& compiles) :
        project(project), pch(pch), sessions(sessions), compiles(compiles) {}

    bool is_open(Fid file) const override;
    std::optional<index::IndexSource> buffer_index(Fid file) const override;
    void for_each_session(llvm::function_ref<bool(const index::IndexSource&)> visit) const override;
    void for_each_session_index(
        llvm::function_ref<bool(const index::TUIndex&)> visit) const override;
    void
        for_each_preamble(llvm::function_ref<bool(const index::IndexSource&)> visit) const override;
    void for_each_overlay(llvm::function_ref<bool(const index::TUIndex&)> visit) const override;
    std::shared_ptr<index::TUIndex> preamble_index(Fid file) const override;

private:
    /// The overlay TU index of an open buffer's PCH, or null when it has
    /// no PCH or the index is unreadable.
    std::shared_ptr<index::TUIndex> overlay_of(Fid file) const;

    /// The open buffer's own file index when it describes the buffer:
    /// the compile is current, or it compiled these very bytes.
    const index::Shard* session_shard(Fid file, const Session& session) const;

    index::IndexSource buffer_source(index::IndexSource::Kind kind,
                                     Fid file,
                                     const Session& session,
                                     const index::Shard& shard) const;

    Project& project;
    PCHFamily& pch;
    const SessionStore& sessions;
    const DocumentCompiles& compiles;
};

}  // namespace clice
