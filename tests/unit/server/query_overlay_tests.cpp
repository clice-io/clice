#include <string>
#include <vector>

#include "test/temp_dir.h"
#include "test/test.h"
#include "test/tester.h"
#include "index/serialization.h"
#include "index/shard.h"
#include "index/tu_index.h"
#include "sched/context.h"
#include "sched/families/pcm.h"
#include "sched/families/turun.h"
#include "sched/graph.h"
#include "sched/index/pump.h"
#include "sched/index/store.h"
#include "server/service/query.h"
#include "server/state/ast_projection.h"
#include "server/state/session_store.h"
#include "worker/pool.h"

#include "kota/ipc/lsp/text.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {
namespace {

TEST_SUITE(QueryOverlay, Tester) {

kota::event_loop loop;
Workspace workspace;
SessionStore session_store;
WorkerPool pool{loop};
ContextResolver resolver{workspace};
TaskGraph graph{loop};
PCMFamily pcm{graph, workspace, resolver, pool};
ASTProjectionTable projections;
IndexStore index_store{loop, workspace};
TURunFamily turun{graph, workspace, resolver, pcm, index_store, pool};
IndexPump indexer{loop, workspace, turun, index_store, pool};
IndexQuery index_query{workspace, session_store, indexer, projections};
IndexQuery agent_query{workspace, session_store, indexer, projections, {.disk_only = true}};

TempDir dir;
index::TUIndex full_index;
std::shared_ptr<Session> session;
std::string main_path;

/// Compile the added sources, persist the preamble envelope (exactly what
/// the PCH build produces), and open a session whose pch_key points at
/// it. The session's own index is the interested-only envelope, mirroring
/// the production per-edit index.
void open_with_overlay(std::source_location location = std::source_location::current()) {
    ASSERT_TRUE(compile());

    full_index = index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit)));
    ASSERT_TRUE(full_index.loaded());

    auto blob_path = dir.path("overlay.pch.idx");
    dir.touch("overlay.pch.idx", index::build_preamble_index(*unit, {}, {}, {}));

    auto& st = workspace.pch_cache["key"];
    st.path = "unused.pch";
    st.index_path = blob_path;
    st.state = nullptr;

    main_path = std::string(full_index.path(full_index.path_count() - 1));
    auto path_id = workspace.path_pool.intern(main_path);
    session = session_store.open(path_id);

    auto it = sources.all_files.find(llvm::sys::path::filename(main_path));
    ASSERT_TRUE(it != sources.all_files.end());
    session->text = it->second.content;
    session->line_starts = kota::ipc::lsp::build_line_starts(session->text);

    auto& entry = projections.entries[path_id];
    auto projection = std::make_shared<ASTProjection>();
    projection->index = std::make_shared<index::TUIndex>(index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit, true))));
    projection->pch_key = "key";
    entry.projection = std::move(projection);
    entry.current = true;
}

index::SymbolHash hash_of(llvm::StringRef name,
                          std::source_location location = std::source_location::current()) {
    index::SymbolHash hash = 0;
    std::uint32_t count = 0;
    full_index.iterate_symbols(
        [&](index::SymbolHash symbol_id, const index::SymbolIdentity& symbol, llvm::StringRef) {
            if(symbol.name == name) {
                hash = symbol_id;
                count += 1;
            }
            return true;
        });
    EXPECT_EQ(count, 1);
    return hash;
}

std::string header_path(llvm::StringRef basename) {
    for(std::uint32_t i = 0; i < full_index.path_count(); i += 1) {
        if(llvm::sys::path::filename(full_index.path(i)) == basename)
            return std::string(full_index.path(i));
    }
    return {};
}

/// Merge the full envelope into the workspace's disk index, installing
/// each section's blob verbatim as background indexing would.
void merge_disk_index() {
    llvm::SmallVector<std::uint32_t> file_ids_map;
    for(std::uint32_t i = 0; i < full_index.path_count(); i += 1) {
        file_ids_map.push_back(workspace.path_pool.intern(full_index.path(i)));
    }
    ASSERT_TRUE(workspace.project_index.merge(full_index, file_ids_map));

    for(std::uint32_t section = 0; section < full_index.section_count(); section += 1) {
        auto local_id = full_index.section_path(section);
        workspace.shards[file_ids_map[local_id]] = index::Shard::from_buffer(
            llvm::MemoryBuffer::getMemBufferCopy(full_index.section_blob(section)));
    }
}

/// A settled, rows-empty per-edit index — what a session holds when every
/// row of its buffer lives behind the PCH. `loaded` is what the freshness
/// gate keys on; an unloaded index means "compile not settled".
index::TUIndex empty_session_index() {
    // Field order MUST mirror the envelope layout (tu_index.cpp).
    struct EnvelopeMirror {
        std::uint32_t format_version = index::index_format_version;
        std::int64_t built_at = 1;
        std::vector<std::string> paths;
    };

    EnvelopeMirror mirror;
    mirror.paths = {main_path};
    auto bytes = kota::codec::fbs::to_bytes(mirror);
    if(!bytes) {
        return {};
    }
    return index::TUIndex::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(
        llvm::StringRef(reinterpret_cast<const char*>(bytes->data()), bytes->size())));
}

void install_empty_index(std::source_location location = std::source_location::current()) {
    auto index = std::make_shared<index::TUIndex>(empty_session_index());
    ASSERT_TRUE(index->loaded());
    auto& entry = projections.entries[session->path_id];
    auto next = ASTProjection(*entry.projection);
    next.index = std::move(index);
    entry.projection = std::make_shared<const ASTProjection>(std::move(next));
}

protocol::Position position_of(llvm::StringRef name) {
    auto pos = session->line_map().to_position(point(name));
    return pos ? *pos : protocol::Position{};
}

TEST_CASE(DefinitionFromOverlayOnly) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦§(ref)foo⟧(); return 0; }
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
inline void §(def)⟦foo⟧() {}
inline void bar() { §(href)⟦§(href)foo⟧(); }
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦§(ref)foo⟧(); return 0; }
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
    add_main("main.cpp", R"(#define §(macro)⟦§(macro)FOO⟧ 1
int main() { return 0; }
)");
    open_with_overlay();

    // Production per-edit indexes never see the preamble region (the PCH
    // swallows it); emulate that by emptying the session's own index so
    // the cursor can only resolve through the overlay's main-file entry.
    install_empty_index();

    auto uri = std::string("file://") + main_path;
    auto info = index_query.lookup_symbol(uri, main_path, position_of("macro"), session.get());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->name, "FOO");
}

TEST_CASE(OverlaySymbolInfo) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
inline void §(bardef)⟦bar⟧() { foo(); }
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦foo⟧(); return 0; }
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
inline void §(def)⟦foo⟧() {}
inline void bar() { §(href)⟦foo⟧(); }
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦§(ref)foo⟧(); return 0; }
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
inline void §(def)⟦callee⟧() {}
inline void caller() { §(call)⟦callee⟧(); }
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(mcall)⟦§(mcall)callee⟧(); return 0; }
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
struct §(b)⟦Base⟧ {};
)");
    add_file("derived.h", R"(
#include "base.h"
struct §(d)⟦Derived⟧ : Base {};
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
inline void §(def)⟦foo⟧() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦foo⟧(); return 0; }
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
    // The π comment keeps the file non-ASCII, so its content is stored in
    // the blob and the text path serves without touching the disk.
    add_main("main.cpp", R"(// π
#define §(macro)⟦FOO⟧ 1
int main() { return 0; }
)");
    ASSERT_TRUE(compile());
    full_index = index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit)));
    ASSERT_TRUE(full_index.loaded());
    merge_disk_index();

    // Macro Definition relations carry the full #define extent, so the
    // agentic text path works for macros through the disk index.
    auto text = agent_query.get_definition_text(hash_of("FOO"));
    ASSERT_TRUE(text.has_value());
    EXPECT_TRUE(llvm::StringRef(text->text).contains("FOO"));
}

TEST_CASE(AsciiPreviewDegrades) {
    // Pure-ASCII blobs re-read the disk for previews. This file only
    // exists in the test VFS, so the read fails like a moved-on file:
    // definition text degrades to nothing while references keep serving
    // their positions, only without context lines.
    add_main("main.cpp", R"(#define §(macro)⟦FOO⟧ 1
int use = FOO;
int main() { return 0; }
)");
    ASSERT_TRUE(compile());
    full_index = index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit)));
    ASSERT_TRUE(full_index.loaded());
    merge_disk_index();

    auto foo = hash_of("FOO");
    EXPECT_FALSE(agent_query.get_definition_text(foo).has_value());

    auto references = agent_query.collect_references(foo, RelationKind::Reference);
    ASSERT_FALSE(references.empty());
    for(auto& reference: references) {
        EXPECT_TRUE(reference.context.empty());
    }
}

TEST_CASE(AsciiPreviewFromDisk) {
    // The ASCII preview happy path: the blob omits the text, the disk
    // still holds the exact bytes, so definition text and context lines
    // serve from the re-read; once the file moves on, the hash check
    // degrades both back to positions-only.
    llvm::StringRef text = "int value = 1;\nint other = value;\n";
    dir.touch("preview.cpp", text);
    auto path = dir.path("preview.cpp");
    auto path_id = workspace.path_pool.intern(path);

    index::SymbolHash sym = 777;
    index::FileIndex rows;
    rows.occurrences.push_back({
        {4, 9},
        sym
    });
    index::Relation def{
        .kind = RelationKind::Definition,
        .range = {4, 9}
    };
    def.set_definition_range({0, 14});
    rows.relations[sym].push_back(def);
    rows.relations[sym].push_back({
        .kind = RelationKind::Reference,
        .range = {27, 32},
        .target_symbol = 0
    });

    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_shard(rows, {}, text, os);
    workspace.shards[path_id] =
        index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
    ASSERT_TRUE(workspace.shards[path_id].ascii());
    workspace.project_index.symbols[sym].name = "value";
    workspace.project_index.symbols[sym].reference_files.add(path_id);

    auto definition = agent_query.get_definition_text(sym);
    ASSERT_TRUE(definition.has_value());
    EXPECT_EQ(definition->text, "int value = 1;");

    auto references = agent_query.collect_references(sym, RelationKind::Reference);
    ASSERT_FALSE(references.empty());
    EXPECT_EQ(references.front().context, "int other = value;");

    dir.touch("preview.cpp", "int moved = 0;\n");
    EXPECT_FALSE(agent_query.get_definition_text(sym).has_value());
    references = agent_query.collect_references(sym, RelationKind::Reference);
    ASSERT_FALSE(references.empty());
    EXPECT_TRUE(references.front().context.empty());
}

TEST_CASE(SharedPreambleScoped) {
    add_main("main.cpp", R"(#define §(macro)⟦§(macro)FOO⟧ 1
#if FOO
#endif
int main() { return 0; }
)");
    open_with_overlay();
    install_empty_index();

    // A second file with a byte-identical preamble shares the PCH (the
    // key excludes the source path), but the preamble entry carries
    // file-local macro identities — its rows must stay scoped to the
    // file that built the blob.
    auto other_path = std::string(llvm::sys::path::parent_path(main_path)) + "/other.cpp";
    auto other = session_store.open(workspace.path_pool.intern(other_path));
    other->text = session->text;
    other->line_starts = session->line_starts;
    auto& other_entry = projections.entries[other->path_id];
    auto other_projection = std::make_shared<ASTProjection>();
    other_projection->pch_key = "key";
    other_entry.projection = std::move(other_projection);
    other_entry.current = true;

    auto locations = index_query.query_relations(main_path,
                                                 position_of("macro"),
                                                 RelationKind::Reference,
                                                 session.get());
    ASSERT_EQ(locations.size(), 1);
    EXPECT_TRUE(llvm::StringRef(locations[0].uri).ends_with("main.cpp"));
}

TEST_CASE(DirtyPreambleServed) {
    add_main("main.cpp", R"(#define §(macro)⟦FOO⟧ 1
int main() { return 0; }
)");
    open_with_overlay();
    install_empty_index();

    // Body edits dirty the session but never move preamble rows: as long
    // as the buffer still starts with the blob's preamble text, the
    // entry keeps serving — the prefix comparison is the freshness check.
    projections.entries[session->path_id].current = false;
    session->text += "int more;\n";
    session->line_starts = kota::ipc::lsp::build_line_starts(session->text);
    EXPECT_TRUE(index_query.find_definition_location(hash_of("FOO")).has_value());
}

TEST_CASE(PreambleDriftSkipped) {
    add_main("main.cpp", R"(#define §(macro)⟦FOO⟧ 1
int main() { return 0; }
)");
    open_with_overlay();
    install_empty_index();

    // A deferred PCH rebuild keeps an old blob while the buffer's
    // preamble moved on; once the buffer no longer starts with the blob's
    // stored preamble text, its rows must not be served.
    session->text = "// drift\n" + session->text;
    session->line_starts = kota::ipc::lsp::build_line_starts(session->text);
    EXPECT_FALSE(index_query.find_definition_location(hash_of("FOO")).has_value());
}

TEST_CASE(OverlayOutranksDisk) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦foo⟧(); return 0; }
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
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_shard(fake, {}, "xxx\n", os);
    workspace.shards[header_id] =
        index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
    workspace.project_index.symbols[foo].reference_files.add(header_id);

    auto def_loc = index_query.find_definition_location(foo);
    ASSERT_TRUE(def_loc.has_value());
    EXPECT_EQ(def_loc->range.start.line, 1);
}

TEST_CASE(SynthesizedArtifactSkipped) {
    workspace.config.project.cache_dir = TestVFS::root();
    add_file("header_context/gen.h", R"(
inline void §(def)⟦gen⟧() {}
)");
    add_main("main.cpp", R"(
#include "header_context/gen.h"
int main() { §(ref)⟦gen⟧(); return 0; }
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
