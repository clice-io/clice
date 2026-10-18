module;

#include "modules/prelude.h"

module clice;

import :index.database;
import :project.project;
import :server.editor_context;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(ContextChoices) {

ZEST_CASE(ContextsBlobRoundTrip) {
    // The editor's choices and artifact hosts ride the context choices: a
    // restart attaching an editor gets them back, and the save that
    // committed them resolves the durability ticket taken before it.
    TempDir tmp;
    tmp.touch("host.cpp", "#include \"h.h\"\n");
    tmp.touch("h.h", "int x;\n");
    auto host_path = tmp.path("host.cpp");
    auto header_path = tmp.path("h.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        EditorContext editor{f.project, f.commands, f.project.contexts};
        f.load();
        editor.load();
        auto host = f.project.file_table.intern(Spelling::absolute(host_path));
        auto header = f.project.file_table.intern(Spelling::absolute(header_path));
        editor.selections[header] = Selection{host, 1, "applied", "base"};
        editor.mark_dirty();
        auto ticket = f.project.contexts.ticket;
        f.save();
        ZASSERT(!f.project.contexts.dirty);
        ZASSERT(f.project.contexts.committed_ticket == ticket);
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    EditorContext editor{f.project, f.commands, f.project.contexts};
    f.load();
    editor.load();
    auto host = f.project.file_table.intern(Spelling::absolute(host_path));
    auto header = f.project.file_table.intern(Spelling::absolute(header_path));
    auto* saved = editor.selection(header);
    ZASSERT(saved != nullptr);
    ZASSERT(saved->host_path_id == host);
    ZASSERT(saved->occurrence == std::optional<std::uint32_t>(1));
    ZASSERT(saved->command_hash == "applied");
    ZASSERT(saved->base_hash == "base");
    ZASSERT(!f.project.contexts.dirty);
}

ZEST_CASE(UnownedContextsPassThrough) {
    // A process with no editor (the batch commands) never rewrites the
    // context choices, yet carries their bytes into a database replacing a
    // corrupt one — the user's choices must not die with the old one.
    TempDir tmp;
    std::string bytes =
        R"({"paths":["/a.h"],"contexts":[{"file":0,"host":4294967295,"occurrence":4294967295,"command_hash":"c","base_hash":"b"}],"artifacts":[]})";
    inject_blob(tmp, index::BlobKind::ContextChoices, "contexts", bytes);

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    f.project.mark_artifacts_dirty();
    f.save();
    auto kept = f.project.database->read(index::BlobKind::ContextChoices, "contexts");
    ZASSERT(bool(kept));
    ZASSERT(kept.buffer->getBuffer() == bytes);

    f.reopen_database();
    ZASSERT(f.project.database != nullptr);
    ZASSERT(!bool(f.project.database->read(index::BlobKind::ContextChoices, "contexts")));
    f.save();
    auto rewritten = f.project.database->read(index::BlobKind::ContextChoices, "contexts");
    ZASSERT(bool(rewritten));
    ZASSERT(rewritten.buffer->getBuffer() == bytes);
}

ZEST_CASE(NoContextsNoRewrite) {
    // Nothing loaded, nothing to carry: a replaced database gets no empty
    // context choices, and no save keeps retrying them.
    TempDir tmp;
    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    f.reopen_database();
    f.save();
    ZASSERT(!bool(f.project.database->read(index::BlobKind::ContextChoices, "contexts")));
}

};  // ZEST_SUITE(ContextChoices)

}  // namespace
}  // namespace clice::testing
