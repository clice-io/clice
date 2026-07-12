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
IndexQuery agent_query{workspace, session_store, indexer, {.disk_only = true}};

TempDir dir;
index::TUIndex full_index;
std::shared_ptr<Session> session;
std::string main_path;

/// Compile the added sources, serialize the full TUIndex as a
/// PreambleState blob (exactly what the PCH build produces), and open a
/// session whose pch_key points at it. The session's own file index is
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
    session->pch_key = "key";
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

void setup() {
    // skip_stale_contribution dereferences this optional; a default
    // Workspace leaves it empty (apply_defaults never runs in tests).
    workspace.config.project.enable_indexing = true;
}

protocol::Position position_of(llvm::StringRef name) {
    auto pos = session->line_map().to_position(point(name));
    return pos ? *pos : protocol::Position{};
}

TEST_CASE(DefinitionFromOverlayOnly) {
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

TEST_CASE(OpenHeaderTargetsExcluded) {
    add_file("base.h", R"(
struct @b[Base] {};
)");
    add_file("derived.h", R"(
#include "base.h"
struct @d[Derived] : Base {};
)");
    add_main("main.cpp", R"(
#include "derived.h"
Derived instance;
)");
    open_with_overlay();

    auto derived = hash_of("Derived");
    auto supertypes = index_query.find_supertypes(derived);
    ASSERT_EQ(supertypes.size(), 1);
    EXPECT_EQ(supertypes[0].name, "Base");

    // Once derived.h is open, its session owns the type relations spelled
    // there; the overlay's disk-snapshot rows must stop contributing.
    session_store.open(workspace.path_pool.intern(header_path("derived.h")));
    supertypes = index_query.find_supertypes(derived);
    EXPECT_EQ(supertypes.size(), 0);
}

TEST_CASE(StaleHeaderSuppressed) {
    add_file("foo.h", R"(
inline void @def[foo]() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { @ref[foo](); return 0; }
)");
    open_with_overlay();

    ASSERT_TRUE(index_query.find_definition_location(hash_of("foo")).has_value());

    // The header's own disk content changed and awaits reindexing: its
    // overlay rows describe text that no longer exists (freshness
    // contract, clause 2), exactly like a shard contribution.
    indexer.enqueue(workspace.path_pool.intern(header_path("foo.h")),
                    ReindexReason::ContentChanged);
    EXPECT_FALSE(index_query.find_definition_location(hash_of("foo")).has_value());
}

TEST_CASE(MacroDefinitionText) {
    add_main("main.cpp", R"(#define @macro[FOO] 1
int main() { return 0; }
)");
    ASSERT_TRUE(compile());
    full_index = index::TUIndex::build(*unit);
    merge_disk_index();

    // Macro Definition relations carry the full #define extent, so the
    // agentic text path works for macros through the disk index.
    auto text = agent_query.get_definition_text(hash_of("FOO"));
    ASSERT_TRUE(text.has_value());
    EXPECT_TRUE(llvm::StringRef(text->text).contains("FOO"));
}

TEST_CASE(SharedPreambleScoped) {
    add_main("main.cpp", R"(#define @macro[$(macro)FOO] 1
#if FOO
#endif
int main() { return 0; }
)");
    open_with_overlay();
    session->file_index = index::FileIndex();
    session->symbols = index::SymbolTable();

    // A second file with a byte-identical preamble shares the PCH (the
    // key excludes the source path), but the preamble entry carries
    // file-local macro identities — its rows must stay scoped to the
    // file that built the blob.
    auto other_path = std::string(llvm::sys::path::parent_path(main_path)) + "/other.cpp";
    auto other = session_store.open(workspace.path_pool.intern(other_path));
    other->text = session->text;
    other->line_starts = session->line_starts;
    other->ast_dirty = false;
    other->pch_key = session->pch_key;

    auto locations = index_query.query_relations(main_path,
                                                 position_of("macro"),
                                                 RelationKind::Reference,
                                                 session.get());
    ASSERT_EQ(locations.size(), 1);
    EXPECT_TRUE(llvm::StringRef(locations[0].uri).ends_with("main.cpp"));
}

TEST_CASE(DirtyPreambleServed) {
    add_main("main.cpp", R"(#define @macro[FOO] 1
int main() { return 0; }
)");
    open_with_overlay();
    session->file_index = index::FileIndex();
    session->symbols = index::SymbolTable();

    // Body edits dirty the session but never move preamble rows: as long
    // as the buffer still starts with the blob's preamble text, the
    // entry keeps serving — the prefix comparison is the freshness check.
    session->ast_dirty = true;
    session->text += "int more;\n";
    session->line_starts = kota::ipc::lsp::build_line_starts(session->text);
    EXPECT_TRUE(index_query.find_definition_location(hash_of("FOO")).has_value());
}

TEST_CASE(PreambleDriftSkipped) {
    add_main("main.cpp", R"(#define @macro[FOO] 1
int main() { return 0; }
)");
    open_with_overlay();
    session->file_index = index::FileIndex();
    session->symbols = index::SymbolTable();

    // A deferred PCH rebuild keeps an old blob while the buffer's
    // preamble moved on; once the buffer no longer starts with the blob's
    // stored preamble text, its rows must not be served.
    session->text = "// drift\n" + session->text;
    session->line_starts = kota::ipc::lsp::build_line_starts(session->text);
    EXPECT_FALSE(index_query.find_definition_location(hash_of("FOO")).has_value());
}

TEST_CASE(OverlayOutranksDisk) {
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
