#pragma once

#include "index/query.h"
#include "project/project.h"
#include "sched/families/pch.h"
#include "sched/index/pump.h"
#include "server/state/ast_projection.h"
#include "server/state/session_store.h"

namespace clice {

/// The server's live index sources: the open buffers' own file indexes
/// (current per the AST projections), the PCH overlays their preambles
/// were compiled into, and the preamble regions those overlays indexed.
class ServerLiveSources final : public index::LiveSources {
public:
    ServerLiveSources(Project& project,
                      PCHFamily& pch,
                      const SessionStore& sessions,
                      const ASTProjectionTable& projections) :
        project(project), pch(pch), sessions(sessions), projections(projections) {}

    bool is_open(Fid file) const override;
    std::optional<index::RowSource> claim(Fid file) const override;
    void each_session(llvm::function_ref<bool(const index::RowSource&)> visit) const override;
    void each_session_index(llvm::function_ref<bool(const index::TUIndex&)> visit) const override;
    void each_preamble(llvm::function_ref<bool(const index::RowSource&)> visit) const override;
    void each_overlay(llvm::function_ref<bool(const index::TUIndex&)> visit) const override;
    bool excluded(llvm::StringRef path) const override;
    std::shared_ptr<index::TUIndex> preamble_blob(Fid file) const override;

private:
    /// The overlay envelope of an open buffer's PCH, or null when it has
    /// no PCH or the envelope is unreadable.
    std::shared_ptr<index::TUIndex> overlay_of(Fid file) const;

    index::RowSource buffer_source(index::RowSource::Kind kind,
                                   Fid file,
                                   const Session& session,
                                   const index::Shard& rows) const;

    Project& project;
    PCHFamily& pch;
    const SessionStore& sessions;
    const ASTProjectionTable& projections;
};

/// Freshness clause 2 as the indexer sees it: a file whose own content
/// changed contributes nothing until its reindex lands. With background
/// indexing disabled nothing ever catches up, so the last-known rows keep
/// serving instead of leaving a permanent hole.
class PumpGate final : public index::FreshnessGate {
public:
    PumpGate(const IndexPump& pump, const Config& config) : pump(pump), config(config) {}

    bool withhold(Fid file) const override {
        return config.project.enable_indexing.value &&
               pump.pending_reason(file) == ReindexReason::ContentChanged;
    }

private:
    const IndexPump& pump;
    const Config& config;
};

}  // namespace clice
