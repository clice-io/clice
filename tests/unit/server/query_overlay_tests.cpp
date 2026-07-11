#include <string>
#include <vector>

#include "test/temp_dir.h"
#include "test/test.h"
#include "test/tester.h"
#include "index/preamble_state.h"
#include "server/compiler/context_resolver.h"
#include "server/compiler/indexer.h"
#include "server/service/query.h"
#include "server/state/session_store.h"
#include "server/worker/worker_pool.h"
#include "syntax/scan.h"

#include "kota/ipc/lsp/text.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::testing {
namespace {

TEST_SUITE(QueryOverlay, Tester) {

kota::event_loop loop;
Workspace workspace;
SessionStore session_store;
WorkerPool pool{loop};
ContextResolver resolver{workspace};
Indexer indexer{loop, workspace, pool, resolver, session_store};
IndexQuery index_query{workspace, session_store, indexer};

TempDir dir;
index::TUIndex full_index;
std::shared_ptr<Session> session;
std::string main_path;

/// Compile the added sources, serialize the full TUIndex as a
/// PreambleState blob (exactly what the PCH build produces), and open a
/// session whose pch_ref points at it. The session's own file index is
/// the interested-only index, mirroring the production per-edit index.
void open_with_overlay(std::source_location location = std::source_location::current()) {
    ASSERT_TRUE(compile());

    full_index = index::TUIndex::build(*unit);
    auto blob_path = dir.path("overlay.pch.idx");
    {
        std::error_code ec;
        llvm::raw_fd_ostream os(blob_path, ec);
        ASSERT_FALSE(bool(ec));
        index::PreambleState::serialize(*unit, full_index, {}, {}, {}, os);
    }

    auto& st = workspace.pch_cache["key"];
    st.path = "unused.pch";
    st.index_path = blob_path;
    st.state = nullptr;

    main_path = full_index.graph.paths.back();
    auto path_id = workspace.path_pool.intern(main_path);
    session = session_store.open(path_id);

    auto it = sources.all_files.find(llvm::sys::path::filename(main_path));
    ASSERT_TRUE(it != sources.all_files.end());
    session->text = it->second.content;
    session->line_starts = kota::ipc::lsp::build_line_starts(session->text);

    auto session_index = index::TUIndex::build(*unit, true);
    session->file_index = std::move(session_index.main_file_index);
    session->symbols = std::move(session_index.symbols);
    session->ast_dirty = false;
    session->pch_ref = Session::PCHRef{"key", compute_preamble_bound(session->text)};
}

index::SymbolHash hash_of(llvm::StringRef name,
                          std::source_location location = std::source_location::current()) {
    index::SymbolHash hash = 0;
    std::uint32_t count = 0;
    for(auto& [symbol_id, symbol]: full_index.symbols) {
        if(symbol.name == name) {
            hash = symbol_id;
            count += 1;
        }
    }
    EXPECT_EQ(count, 1);
    return hash;
}

std::string header_path(llvm::StringRef basename) {
    for(auto& path: full_index.graph.paths) {
        if(llvm::sys::path::filename(path) == basename)
            return path;
    }
    return {};
}

/// Merge the full TUIndex into the workspace's disk index with real
/// contents, as background indexing would.
void merge_disk_index() {
    auto file_ids_map = workspace.project_index.merge(full_index, workspace.path_pool);

    auto content_of = [&](llvm::StringRef path) -> llvm::StringRef {
        auto it = sources.all_files.find(llvm::sys::path::filename(path));
        return it != sources.all_files.end() ? llvm::StringRef(it->second.content)
                                             : llvm::StringRef();
    };

    auto main_tu_path_id = static_cast<std::uint32_t>(full_index.graph.paths.size() - 1);
    llvm::StringRef main_tu_path = full_index.graph.paths[main_tu_path_id];

    llvm::SmallVector<index::DepLocation> deps;
    for(auto& loc: full_index.graph.locations) {
        deps.push_back({full_index.graph.paths[loc.path_id], loc.line, loc.include});
    }
    workspace.merged_indices[file_ids_map[main_tu_path_id]].merge(main_tu_path,
                                                                  full_index.built_at,
                                                                  deps,
                                                                  full_index.main_file_index,
                                                                  content_of(main_tu_path));

    for(auto& [fid, file_idx]: full_index.file_indices) {
        auto tu_pid = full_index.graph.path_id(fid);
        workspace.merged_indices[file_ids_map[tu_pid]].merge(
            main_tu_path,
            full_index.graph.include_location_id(fid),
            file_idx,
            content_of(full_index.graph.paths[tu_pid]));
    }
}

void reset() {
    workspace.project_index = index::ProjectIndex();
    workspace.merged_indices.clear();
    workspace.pch_cache.clear();
    workspace.path_pool = PathPool();
    workspace.config.project.cache_dir = {};
    session_store.sessions.clear();
    session.reset();
    clear();
}

protocol::Position position_of(llvm::StringRef name) {
    auto pos = session->line_map().to_position(point(name));
    return pos ? *pos : protocol::Position{};
}

TEST_CASE(DefinitionFromOverlayOnly) {
    reset();
    add_file("foo.h", R"(
inline void @def[foo]() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { @ref[$(ref)foo](); return 0; }
)");
    open_with_overlay();

    // No disk index at all — the in-memory-file case: the overlay is the
    // only source that knows where foo is defined.
    auto locations = index_query.query_relations(main_path,
                                                 position_of("ref"),
                                                 RelationKind::Definition,
                                                 session.get());
    ASSERT_EQ(locations.size(), 1);
    EXPECT_TRUE(llvm::StringRef(locations[0].uri).ends_with("foo.h"));
}

TEST_CASE(ReferencesUnionWithDedup) {
    reset();
    add_file("foo.h", R"(
inline void @def[foo]() {}
inline void bar() { @href[$(href)foo](); }
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { @ref[$(ref)foo](); return 0; }
)");
    open_with_overlay();
    // The header's disk shard and the overlay now both carry the
    // header-internal reference; results must contain it exactly once.
    merge_disk_index();

    auto locations = index_query.query_relations(main_path,
                                                 position_of("ref"),
                                                 RelationKind::Reference,
                                                 session.get());
    ASSERT_EQ(locations.size(), 2);

    std::size_t header_rows = 0;
    std::size_t main_rows = 0;
    for(auto& location: locations) {
        if(llvm::StringRef(location.uri).ends_with("foo.h"))
            header_rows += 1;
        if(llvm::StringRef(location.uri).ends_with("main.cpp"))
            main_rows += 1;
    }
    EXPECT_EQ(header_rows, 1);
    EXPECT_EQ(main_rows, 1);
}

TEST_CASE(PreambleMacroCursor) {
    reset();
    add_main("main.cpp", R"(#define @macro[$(macro)FOO] 1
int main() { return 0; }
)");
    open_with_overlay();

    // Production per-edit indexes never see the preamble region (the PCH
    // swallows it); emulate that by emptying the session's own index so
    // the cursor can only resolve through the overlay's main-file entry.
    session->file_index = index::FileIndex();
    session->symbols = index::SymbolTable();

    auto uri = std::string("file://") + main_path;
    auto info = index_query.lookup_symbol(uri, main_path, position_of("macro"), session.get());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->name, "FOO");
}

TEST_CASE(OverlaySymbolInfo) {
    reset();
    add_file("foo.h", R"(
inline void @def[foo]() {}
inline void @bardef[bar]() { foo(); }
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { @ref[foo](); return 0; }
)");
    open_with_overlay();

    // bar is never referenced by the buffer, so neither the session's
    // symbol table nor the (empty) project index knows it — only the
    // overlay's symbol table does.
    auto bar_hash = hash_of("bar");

    std::string name;
    SymbolKind kind;
    ASSERT_TRUE(index_query.find_symbol_info(bar_hash, name, kind));
    EXPECT_EQ(name, "bar");

    auto def_loc = index_query.find_definition_location(bar_hash);
    ASSERT_TRUE(def_loc.has_value());
    EXPECT_TRUE(llvm::StringRef(def_loc->uri).ends_with("foo.h"));
}

TEST_CASE(OpenHeaderExcluded) {
    reset();
    add_file("foo.h", R"(
inline void @def[foo]() {}
inline void bar() { @href[foo](); }
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { @ref[$(ref)foo](); return 0; }
)");
    open_with_overlay();

    // Opening the header makes its session authoritative: overlay rows
    // for it describe the disk snapshot and would map onto the edited
    // buffer at wrong lines, so they must vanish from results.
    session_store.open(workspace.path_pool.intern(header_path("foo.h")));

    auto locations = index_query.query_relations(main_path,
                                                 position_of("ref"),
                                                 RelationKind::Reference,
                                                 session.get());
    ASSERT_EQ(locations.size(), 1);
    EXPECT_TRUE(llvm::StringRef(locations[0].uri).ends_with("main.cpp"));
}

TEST_CASE(IncomingCallsDedup) {
    reset();
    add_file("foo.h", R"(
inline void @def[callee]() {}
inline void caller() { @call[callee](); }
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { @mcall[$(mcall)callee](); return 0; }
)");
    open_with_overlay();
    // The header call site now exists in both its disk shard and the
    // overlay; each caller must report it exactly once.
    merge_disk_index();

    auto calls = index_query.find_incoming_calls(hash_of("callee"));
    ASSERT_EQ(calls.size(), 2);
    for(auto& call: calls) {
        EXPECT_EQ(call.from_ranges.size(), 1);
    }
}

TEST_CASE(CollectReferencesDedup) {
    reset();
    add_file("foo.h", R"(
inline void @def[foo]() {}
inline void bar() { @href[foo](); }
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { @ref[foo](); return 0; }
)");
    open_with_overlay();
    merge_disk_index();

    auto refs = index_query.collect_references(hash_of("foo"), RelationKind::Reference);
    std::size_t header_rows = 0;
    for(auto& ref: refs) {
        if(llvm::StringRef(ref.file).ends_with("foo.h")) {
            header_rows += 1;
            EXPECT_TRUE(llvm::StringRef(ref.context).contains("bar"));
        }
    }
    EXPECT_EQ(header_rows, 1);
}

TEST_CASE(DefinitionTextFromOverlay) {
    reset();
    add_file("foo.h", R"(
inline void @def[foo]() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { @ref[foo](); return 0; }
)");
    open_with_overlay();

    auto text = index_query.get_definition_text(hash_of("foo"));
    ASSERT_TRUE(text.has_value());
    EXPECT_TRUE(llvm::StringRef(text->file).ends_with("foo.h"));
    EXPECT_TRUE(llvm::StringRef(text->text).contains("void foo"));
}

TEST_CASE(DirtyMainEntrySkipped) {
    reset();
    add_main("main.cpp", R"(#define @macro[FOO] 1
int main() { return 0; }
)");
    open_with_overlay();
    session->file_index = index::FileIndex();
    session->symbols = index::SymbolTable();

    // A dirty session's buffer coordinates are untrustworthy; the overlay
    // main-file entry must be gated exactly like the session index.
    session->ast_dirty = true;
    EXPECT_FALSE(index_query.find_definition_location(hash_of("FOO")).has_value());

    session->ast_dirty = false;
    EXPECT_TRUE(index_query.find_definition_location(hash_of("FOO")).has_value());
}

TEST_CASE(PreambleDriftSkipped) {
    reset();
    add_main("main.cpp", R"(#define @macro[FOO] 1
int main() { return 0; }
)");
    open_with_overlay();
    session->file_index = index::FileIndex();
    session->symbols = index::SymbolTable();

    // A deferred PCH rebuild keeps an old pch_ref while the buffer's
    // preamble moved on; the blob's main-file rows must not be served
    // against the drifted buffer.
    session->pch_ref = Session::PCHRef{"key", session->pch_ref->bound + 7};
    EXPECT_FALSE(index_query.find_definition_location(hash_of("FOO")).has_value());
}

TEST_CASE(OverlayOutranksDisk) {
    reset();
    add_file("foo.h", R"(
inline void @def[foo]() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { @ref[foo](); return 0; }
)");
    open_with_overlay();

    // Fabricate a divergent disk row: another context's shard claims the
    // definition sits on line 0. The overlay (live context) must win.
    auto foo = hash_of("foo");
    index::FileIndex fake;
    index::Relation relation{
        .kind = RelationKind::Definition,
        .range = {0, 3}
    };
    relation.set_definition_range({0, 3});
    fake.relations[foo].push_back(relation);
    auto header_id = workspace.path_pool.intern(header_path("foo.h"));
    workspace.merged_indices[header_id].merge("other_tu", 0, fake, "xxx\n");
    workspace.project_index.symbols[foo].reference_files.add(header_id);

    auto def_loc = index_query.find_definition_location(foo);
    ASSERT_TRUE(def_loc.has_value());
    EXPECT_EQ(def_loc->range.start.line, 1);
}

TEST_CASE(SynthesizedArtifactSkipped) {
    reset();
    workspace.config.project.cache_dir = TestVFS::root();
    add_file("header_context/gen.h", R"(
inline void @def[gen]() {}
)");
    add_main("main.cpp", R"(
#include "header_context/gen.h"
int main() { @ref[gen](); return 0; }
)");
    open_with_overlay();

    // The header lives inside the synthesized-artifact directory: its
    // overlay rows must never send the user into the cache.
    EXPECT_FALSE(index_query.find_definition_location(hash_of("gen")).has_value());
}

TEST_CASE(UnreadableBlobCleared) {
    reset();
    dir.touch("junk.pch.idx", "not a flatbuffer");

    PCHState st;
    st.index_path = dir.path("junk.pch.idx");
    EXPECT_TRUE(st.load_state() == nullptr);
    // The cleared path makes the pair look incomplete, so the next
    // ensure_pch round rebuilds it instead of retrying the mmap forever.
    EXPECT_TRUE(st.index_path.empty());
}

};  // TEST_SUITE(QueryOverlay)

}  // namespace
}  // namespace clice::testing
