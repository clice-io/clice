module;

#include "support/prelude.h"

module clice;

import :index.database;
import :index.format;
import :project.project;
import :support.bytes;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.file_system;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(ArtifactRecords) {

ZEST_CASE(StaleFormatDropsPch) {
    // A .pch.idx index written under an older index format is
    // unreadable; the format gate drops every PCH entry at load so the
    // pairs rebuild immediately, instead of the mismatch showing up lazily
    // on the first overlay query — which cannot trigger a rebuild.
    TempDir tmp;
    tmp.touch("dep.h", "int x;\n");
    auto dep_path = tmp.path("dep.h");
    std::string artifacts;

    auto setup = [&](IndexerFixture& f) {
        open_store(tmp, f.project);
        f.project.store->register_namespace(
            {.name = "pch", .extension = ".pch", .aux_extension = ".pch.idx"});
    };

    {
        IndexerFixture f;
        setup(f);
        auto pending = f.project.store->begin_store("pch", "k");
        ZASSERT(!vfs::write(pending.tmp_path, "pch-bytes"));
        ZASSERT(f.project.store->commit(std::move(pending)));
        auto aux = f.project.store->begin_store_aux("pch", "k");
        ZASSERT(!vfs::write(aux.tmp_path, "idx-bytes"));
        ZASSERT(f.project.store->commit(std::move(aux)));

        auto dep_id = f.project.file_table.intern(Spelling::absolute(dep_path));
        auto& st = f.project.pch_cache["k"];
        st.blob = "k";
        st.path = "k.pch";
        st.deps.push_back(
            {.path_id = dep_id, .version = f.project.file_table.intern_version(dep_id, 7)});
        f.project.mark_artifacts_dirty();
        f.save();

        auto blob = f.project.database->read(index::BlobKind::ArtifactRecords, "artifacts");
        ZASSERT(bool(blob));
        artifacts = blob.buffer->getBuffer().str();
    }

    {
        // The premise: under the current format the entry loads.
        IndexerFixture f;
        setup(f);
        f.load();
        ZASSERT(f.project.pch_cache.size() == std::size_t(1));

        // Put back the blob as an older binary would have written it.
        auto current = std::format("\"pch_index_format\":{}", index::index_format_version);
        auto stale = std::format("\"pch_index_format\":{}", index::index_format_version - 1);
        auto pos = artifacts.find(current);
        ZASSERT(pos != std::string::npos);
        artifacts.replace(pos, current.size(), stale);
        index::BlobDatabase::Blob blob{index::BlobKind::ArtifactRecords,
                                       "artifacts",
                                       copy_bytes(artifacts)};
        ZASSERT(f.project.database->write(blob, {}));
    }

    IndexerFixture f;
    setup(f);
    f.load();
    ZASSERT(f.project.pch_cache.empty());
}

ZEST_CASE(DeplessPcmDropped) {
    // A dep-less PCM entry is an unvalidatable snapshot (the key embeds
    // no content): trusted, it would blindly serve a stale PCM after an
    // offline edit. Load drops it so the module rebuilds once.
    TempDir tmp;
    tmp.touch("mod.cppm", "export module m;\n");
    auto src = tmp.path("mod.cppm");

    auto setup = [&](IndexerFixture& f) {
        open_store(tmp, f.project);
        f.project.store->register_namespace({.name = "pcm", .extension = ".pcm"});
    };

    {
        IndexerFixture f;
        setup(f);
        auto pending = f.project.store->begin_store("pcm", "k");
        ZASSERT(!vfs::write(pending.tmp_path, "pcm-bytes"));
        ZASSERT(f.project.store->commit(std::move(pending)));

        auto& st = f.project.pcm_builds[f.project.file_table.intern(Spelling::absolute(src))];
        st.path = "m.pcm";
        st.key = "k";
        st.blob = "k";
        f.project.mark_artifacts_dirty();
        f.save();

        // The premise: the dep-less entry was persisted, so the tail
        // assertion exercises the load-side drop, not a write-side skip.
        auto blob = f.project.database->read(index::BlobKind::ArtifactRecords, "artifacts");
        ZASSERT(bool(blob));
        ZASSERT(blob.buffer->getBuffer().contains("\"key\":\"k\""));
    }

    IndexerFixture f;
    setup(f);
    f.load();
    ZASSERT(f.project.pcm_builds.empty());
}

ZEST_CASE(HeaderModePersisted) {
    // The NeedsContext result rides the artifact records: a restart with
    // unchanged disk content adopts it instead of re-running the trial.
    TempDir tmp;
    tmp.touch("utils.h", "inline int f();\n");
    auto path = tmp.path("utils.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto id = f.project.file_table.intern(Spelling::absolute(path));
        auto disk = f.project.file_table.disk.current(id);
        ZASSERT(disk);
        f.commands.record_header_mode(id, HeaderMode::NeedsContext, disk->hash);
        f.project.mark_artifacts_dirty();
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    auto id = f.project.file_table.intern(Spelling::absolute(path));
    ZASSERT(f.commands.header_mode(id) == HeaderMode::NeedsContext);
}

ZEST_CASE(HeaderModesOfOlderRecords) {
    // Records an older binary wrote name each header's mode beside it, and
    // still load whole.
    TempDir tmp;
    tmp.touch("utils.h", "inline int f();\n");
    auto path = tmp.path("utils.h");
    std::string artifacts;

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto id = f.project.file_table.intern(Spelling::absolute(path));
        auto disk = f.project.file_table.disk.current(id);
        ZASSERT(disk);
        f.commands.record_header_mode(id, HeaderMode::NeedsContext, disk->hash);
        f.project.mark_artifacts_dirty();
        f.save();
        auto blob = f.project.database->read(index::BlobKind::ArtifactRecords, "artifacts");
        ZASSERT(bool(blob));
        artifacts = blob.buffer->getBuffer().str();
    }

    {
        // The records as the older binary wrote them, under the path this
        // one stored.
        auto begin = artifacts.find(R"("paths":[)");
        ZASSERT(begin != std::string::npos);
        auto end = artifacts.find(']', begin);
        auto hash = artifacts.find(R"("content_hash":)");
        ZASSERT(hash != std::string::npos);
        auto hash_end = artifacts.find('}', hash);
        auto older = std::format(
            R"({{{},"pch_index_format":{},"pch":[],"pcm":[],"header_modes":[{{"file":0,"mode":2,{}}}]}})",
            artifacts.substr(begin, end + 1 - begin),
            index::index_format_version,
            artifacts.substr(hash, hash_end - hash));
        IndexerFixture f;
        open_store(tmp, f.project);
        index::BlobDatabase::Blob blob{index::BlobKind::ArtifactRecords,
                                       "artifacts",
                                       copy_bytes(older)};
        ZASSERT(f.project.database->write(blob, {}));
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    auto id = f.project.file_table.intern(Spelling::absolute(path));
    ZASSERT(f.commands.header_mode(id) == HeaderMode::NeedsContext);
}

};  // ZEST_SUITE(ArtifactRecords)

}  // namespace
}  // namespace clice::testing
