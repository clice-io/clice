module;

#include "modules/prelude.h"

module clice;

import :index.hierarchy;
import :index.indexer;
import :index.query;
import :index.shard;
import :index.text;
import :index.tu_index;
import :project.command_resolver;
import :project.index_store;
import :sched.families.pch;
import :sched.families.pcm;
import :sched.families.turun;
import :sched.graph;
import :sched.index.pump;
import :server.ast_projection;
import :server.live_sources;
import :server.session_store;
import :unittests.utils.temp_dir;
import :unittests.utils.tester;
import :unittests.utils.tu_index_blob;
import :unittests.utils;
import :vfs.file_system;
import :vfs.path;
import :worker.pool;

namespace clice::testing {
namespace {

ZEST_SUITE(QueryOverlay, Tester) {

kota::event_loop loop;
FileTable files;
Project project{files};
SessionStore session_store;
WorkerPool pool{loop};
CommandResolver resolver{project};
TaskGraph graph;
PCMFamily pcm{graph, project, resolver, pool};
ASTProjectionTable projections;
IndexStore index_store{loop, project, resolver};
PCHFamily pch{graph, project, pool};
TURunFamily turun{graph, project, resolver, pcm, pch, index_store, pool};
IndexPump indexer{loop, project, turun, index_store, pool};
ServerLiveSources live{project, pch, session_store, projections};
index::FreshnessGate gate{project.file_table};
index::IndexQuery index_query{project.index, project.file_table, &gate, &live};
index::IndexQuery disk_query{project.index, project.file_table, &gate, nullptr};

TempDir dir;
index::TUIndex full_index;
std::shared_ptr<Session> session;
std::string focus_path;

/// Compile the added sources, persist the preamble index (exactly what
/// the PCH build produces), and open a session whose pch_key points at
/// it. The session's own index is the focus file's TU index of an open
/// file's compile, mirroring the production per-edit index.
void open_with_overlay(std::source_location location = std::source_location::current()) {
    kind = CompilationKind::Whole;
    ZASSERT(compile());

    full_index = index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit)));
    ZASSERT(full_index.loaded());

    auto blob_path = dir.path("overlay.pch.idx");
    dir.touch("overlay.pch.idx", index::build_preamble_index(*unit, {}, {}, {}, {}));

    kind = CompilationKind::Content;
    ZASSERT(compile());

    auto& st = project.pch_cache["key"];
    st.path = "unused.pch";
    st.index_path = blob_path;
    st.state = nullptr;

    focus_path = std::string(full_index.path(full_index.path_count() - 1));
    auto path_id = project.file_table.intern(Spelling::absolute(focus_path));
    session = session_store.open(path_id);

    auto it = sources.all_files.find(llvm::sys::path::filename(focus_path));
    ZASSERT(it != sources.all_files.end());
    session->set_text(it->second.content);

    auto& entry = projections.entries[path_id];
    auto projection = std::make_shared<ASTProjection>();
    projection->index = std::make_shared<index::TUIndex>(index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit))));
    projection->pch_key = "key";
    entry.projection = std::move(projection);
    entry.current = true;
}

index::SymbolHash hash_of(llvm::StringRef name,
                          std::source_location location = std::source_location::current()) {
    index::SymbolHash hash = 0;
    std::uint32_t count = 0;
    full_index.for_each_symbol([&](index::SymbolHash symbol_id,
                                   const index::SymbolView& symbol,
                                   llvm::ArrayRef<std::uint32_t>) {
        if(symbol.name == name) {
            hash = symbol_id;
            count += 1;
        }
        return true;
    });
    ZEXPECT(count == 1);
    return hash;
}

std::string header_path(llvm::StringRef basename) {
    for(std::uint32_t i = 0; i < full_index.path_count(); i += 1) {
        if(llvm::sys::path::filename(full_index.path(i)) == basename)
            return std::string(full_index.path(i));
    }
    return {};
}

/// Merge the full TU index into the workspace's disk index, installing
/// each section's blob verbatim as background indexing would;
/// `focus_on_disk` names the focus file by a copy of it on disk instead.
void merge_disk_index(llvm::StringRef focus_on_disk = {}) {
    llvm::SmallVector<Fid> file_ids_map;
    for(std::uint32_t i = 0; i < full_index.path_count(); i += 1) {
        auto focus = i + 1 == full_index.path_count() && !focus_on_disk.empty();
        file_ids_map.push_back(project.file_table.intern(
            Spelling::absolute(focus ? focus_on_disk : full_index.path(i))));
    }
    project.index.merge_symbols(full_index, file_ids_map);

    for(std::uint32_t section = 0; section < full_index.section_count(); section += 1) {
        auto local_id = full_index.section_path(section);
        project.index.shards[file_ids_map[local_id]] = index::Shard::from_buffer(
            llvm::MemoryBuffer::getMemBufferCopy(full_index.section_blob(section)));
    }
}

/// A finished per-edit index with no entries — what a session holds when
/// every entry of its buffer lives behind the PCH. `loaded` is what the
/// freshness gate keys on; an unloaded index means "compile not finished".
index::TUIndex empty_session_index() {
    auto blob = tu_index_blob();
    blob.built_at = 1;
    blob.paths = {focus_path};
    return index::TUIndex::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(tu_index_bytes(blob)));
}

void install_empty_index(std::source_location location = std::source_location::current()) {
    auto index = std::make_shared<index::TUIndex>(empty_session_index());
    ZASSERT(index->loaded());
    auto& entry = projections.entries[session->path_id];
    auto next = ASTProjection(*entry.projection);
    next.index = std::move(index);
    entry.projection = std::make_shared<const ASTProjection>(std::move(next));
}

/// The symbol under a marker in the open session, through the query's
/// serving source (buffer coordinates).
index::IndexQuery::Cursor
    cursor_of(llvm::StringRef name,
              std::source_location location = std::source_location::current()) {
    auto cursor = index_query.symbol_at(session->path_id, point(name));
    ZEXPECT(cursor);
    return cursor.value_or(index::IndexQuery::Cursor{});
}

/// The sites carrying `kind` for the symbol under a marker.
std::vector<index::Site> relations(llvm::StringRef name, RelationKind kind) {
    auto cursor = cursor_of(name);
    return index_query.sites(cursor.symbols.front(), cursor.site.file, kind);
}

/// The 0-based line a site starts on.
std::uint32_t line_of(const index::Site& site) {
    return site.begin.line;
}

ZEST_CASE(DefinitionFromOverlayOnly) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
)");
    add_focus("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦§(ref)foo⟧(); return 0; }
)");
    open_with_overlay();

    // No disk index at all — the in-memory-file case: the overlay is the
    // only source that knows where foo is defined.
    auto locations = relations("ref", RelationKind::Definition);
    ZASSERT(locations.size() == 1);
    ZEXPECT(locations[0].path.ends_with("foo.h"));
}

ZEST_CASE(ReferencesUnionWithDedup) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
inline void bar() { §(href)⟦§(href)foo⟧(); }
)");
    add_focus("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦§(ref)foo⟧(); return 0; }
)");
    open_with_overlay();
    // The header's disk shard and the overlay now both carry the
    // header-internal reference; results must contain it exactly once.
    merge_disk_index();

    auto locations = relations("ref", RelationKind::Reference);
    ZASSERT(locations.size() == 2);

    std::size_t header_refs = 0;
    std::size_t main_refs = 0;
    for(auto& location: locations) {
        if(location.path.ends_with("foo.h"))
            header_refs += 1;
        if(location.path.ends_with("main.cpp"))
            main_refs += 1;
    }
    ZEXPECT(header_refs == 1);
    ZEXPECT(main_refs == 1);
}

ZEST_CASE(PreambleMacroCursor) {
    add_focus("main.cpp", R"(#define §(macro)⟦§(macro)FOO⟧ 1
int main() { return 0; }
)");
    open_with_overlay();

    // Production per-edit indexes never see the preamble region (the PCH
    // swallows it); emulate that by emptying the session's own index so
    // the cursor can only resolve through the overlay's focus-file entry.
    install_empty_index();

    auto cursor = cursor_of("macro");
    auto info = index_query.symbol_info(cursor.symbols.front());
    ZASSERT(info);
    ZEXPECT(info->name == "FOO");

    // The definition text comes from the same preamble entries, sliced from
    // the buffer: the hover card for a `#define` above the PCH bound.
    auto definition = index::declared_text(index_query,
                                           cursor.symbols.front(),
                                           cursor.site.file,
                                           RelationKind::Definition);
    ZASSERT(definition);
    ZEXPECT(llvm::StringRef(definition->text).contains("FOO"));
}

ZEST_CASE(OverlaySymbolInfo) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
inline void §(bardef)⟦bar⟧() { foo(); }
)");
    add_focus("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦foo⟧(); return 0; }
)");
    open_with_overlay();

    // bar is never referenced by the buffer, so neither the session's
    // symbol table nor the (empty) project index knows it — only the
    // overlay's symbol table does.
    auto bar_hash = hash_of("bar");

    auto info = index_query.symbol_info(bar_hash);
    ZASSERT(info);
    ZEXPECT(info->name == "bar");

    auto def = index_query.first_site(bar_hash, Fid{}, RelationKind::Definition);
    ZASSERT(def);
    ZEXPECT(def->path.ends_with("foo.h"));
}

ZEST_CASE(OpenHeaderExcluded) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
inline void bar() { §(href)⟦foo⟧(); }
)");
    add_focus("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦§(ref)foo⟧(); return 0; }
)");
    open_with_overlay();

    // Opening the header makes its session authoritative: overlay entries
    // for it describe the disk snapshot and would map onto the edited
    // buffer at wrong lines, so they must vanish from results.
    session_store.open(project.file_table.intern(Spelling::absolute(header_path("foo.h"))));

    auto locations = relations("ref", RelationKind::Reference);
    ZASSERT(locations.size() == 1);
    ZEXPECT(locations[0].path.ends_with("main.cpp"));
}

ZEST_CASE(IncomingCallsDedup) {
    add_file("foo.h", R"(
inline void §(def)⟦callee⟧() {}
inline void caller() { §(call)⟦callee⟧(); }
)");
    add_focus("main.cpp", R"(
#include "foo.h"
int main() { §(mcall)⟦§(mcall)callee⟧(); return 0; }
)");
    open_with_overlay();
    // The header call site now exists in both its disk shard and the
    // overlay; each caller must report it exactly once.
    merge_disk_index();

    auto callers =
        index::call_graph(index_query, hash_of("callee"), Fid{}, {.callees = false}).callers;
    ZASSERT(callers.size() == 2);
    for(auto& edge: callers) {
        ZEXPECT(edge.sites.size() == 1);
    }
}

ZEST_CASE(OpenHeaderTargetsExcluded) {
    add_file("base.h", R"(
struct §(b)⟦Base⟧ {};
)");
    add_file("derived.h", R"(
#include "base.h"
struct §(d)⟦Derived⟧ : Base {};
)");
    add_focus("main.cpp", R"(
#include "derived.h"
Derived instance;
)");
    open_with_overlay();

    auto derived = hash_of("Derived");
    auto supertypes =
        index::type_hierarchy(index_query, derived, Fid{}, {.subtypes = false}).supertypes;
    ZASSERT(supertypes.size() == 1);
    ZEXPECT(supertypes[0].symbol.name == "Base");

    // Once derived.h is open, its session owns the type relations spelled
    // there; the overlay's disk-snapshot relations must stop contributing.
    session_store.open(project.file_table.intern(Spelling::absolute(header_path("derived.h"))));
    supertypes = index::type_hierarchy(index_query, derived, Fid{}, {.subtypes = false}).supertypes;
    ZEXPECT(supertypes.size() == 0);
}

ZEST_CASE(StaleHeaderSuppressed) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
)");
    add_focus("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦foo⟧(); return 0; }
)");
    open_with_overlay();

    ZASSERT(index_query.first_site(hash_of("foo"), Fid{}, RelationKind::Definition));

    // The disk was seen holding other text for the header: its overlay
    // entries describe text that no longer exists (freshness contract,
    // clause 2), exactly like a shard contribution.
    project.file_table.disk.observe(
        project.file_table.intern(Spelling::absolute(header_path("foo.h"))),
        DiskObservation{.hash = 1});
    ZEXPECT(!index_query.first_site(hash_of("foo"), Fid{}, RelationKind::Definition).has_value());
}

ZEST_CASE(MacroDefinitionText) {
    // Macro Definition relations carry the full #define extent, so the
    // disk text path previews macros — in a non-ASCII file too, whose blob
    // keeps the text of its non-ASCII lines for position mapping alone.
    add_focus("main.cpp", R"(// π
#define §(macro)⟦FOO⟧ 1
int main() { return 0; }
)");
    ZASSERT(compile());
    full_index = index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit)));
    ZASSERT(full_index.loaded());
    dir.touch("main.cpp", sources.all_files.find("main.cpp")->second.content);
    merge_disk_index(dir.path("main.cpp"));

    auto text = index::declared_text(disk_query, hash_of("FOO"), Fid{}, RelationKind::Definition);
    ZASSERT(text);
    ZEXPECT(llvm::StringRef(text->text).contains("FOO"));
}

ZEST_CASE(AsciiPreviewDegrades) {
    // Pure-ASCII blobs re-read the disk for previews. This file only
    // exists in the test VFS, so the read fails like a moved-on file:
    // definition text degrades to nothing while references keep serving
    // their positions, only without context lines.
    add_focus("main.cpp", R"(#define §(macro)⟦FOO⟧ 1
int use = FOO;
int main() { return 0; }
)");
    ZASSERT(compile());
    full_index = index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit)));
    ZASSERT(full_index.loaded());
    merge_disk_index();

    auto foo = hash_of("FOO");
    ZEXPECT(!index::declared_text(disk_query, foo, Fid{}, RelationKind::Definition).has_value());

    auto references = disk_query.sites(foo, Fid{}, RelationKind::Reference);
    ZASSERT(!references.empty());
    for(auto& line: index::context_lines(disk_query, references)) {
        ZEXPECT(line.empty());
    }
}

ZEST_CASE(PreviewFromDisk) {
    // The preview happy path: the disk still holds the exact bytes the
    // shard was built from, so definition text and context lines
    // serve from the re-read; once the file moves on, the hash check
    // degrades both back to positions-only.
    llvm::StringRef text = "int value = 1;\nint other = value;\n";
    dir.touch("preview.cpp", text);
    auto path = dir.path("preview.cpp");
    auto path_id = project.file_table.intern(Spelling::absolute(path));

    index::SymbolHash symbol_hash = 777;
    index::FileIndex file_index;
    file_index.occurrences.push_back({
        {4, 9},
        symbol_hash
    });
    index::Relation def{
        .kind = RelationKind::Definition,
        .range = {4, 9}
    };
    def.set_extent({0, 14});
    file_index.relations[symbol_hash].push_back(def);
    file_index.relations[symbol_hash].push_back({
        .kind = RelationKind::Reference,
        .range = {27, 32},
        .target = 0
    });

    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_shard(file_index, {}, text, os);
    project.index.shards[path_id] =
        index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
    ZASSERT(project.index.shards[path_id].positions().text().empty());
    auto& symbol = project.index.edit(symbol_hash);
    symbol.name = "value";
    symbol.reference_files.add(path_id.raw);

    auto definition =
        index::declared_text(disk_query, symbol_hash, Fid{}, RelationKind::Definition);
    ZASSERT(definition);
    ZEXPECT(definition->text == "int value = 1;");

    auto references = disk_query.sites(symbol_hash, Fid{}, RelationKind::Reference);
    ZASSERT(!references.empty());
    ZEXPECT(index::context_lines(disk_query, references) ==
            std::vector<std::string>{"int other = value;"});

    dir.touch("preview.cpp", "int moved = 0;\n");
    ZEXPECT(!index::declared_text(disk_query, symbol_hash, Fid{}, RelationKind::Definition)
                 .has_value());
    references = disk_query.sites(symbol_hash, Fid{}, RelationKind::Reference);
    ZASSERT(!references.empty());
    ZEXPECT(index::context_lines(disk_query, references) == std::vector<std::string>{""});
}

ZEST_CASE(PreviewDropsLineEnd) {
    // A CRLF file's preview line ends before its "\r\n".
    llvm::StringRef text = "int value = 1;\r\nint other = value;\r\n";
    dir.touch("crlf.cpp", text);
    auto path_id = project.file_table.intern(Spelling::absolute(dir.path("crlf.cpp")));

    index::SymbolHash symbol_hash = 778;
    index::FileIndex file_index;
    file_index.relations[symbol_hash].push_back({
        .kind = RelationKind::Reference,
        .range = {28, 33},
        .target = 0
    });
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_shard(file_index, {}, text, os);
    project.index.shards[path_id] =
        index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
    project.index.edit(symbol_hash).reference_files.add(path_id.raw);

    auto references = disk_query.sites(symbol_hash, Fid{}, RelationKind::Reference);
    ZASSERT(references.size() == 1U);
    ZEXPECT(index::context_lines(disk_query, references) ==
            std::vector<std::string>{"int other = value;"});
}

ZEST_CASE(SharedPreambleScoped) {
    add_focus("main.cpp", R"(#define §(macro)⟦§(macro)FOO⟧ 1
#if FOO
#endif
int main() { return 0; }
)");
    open_with_overlay();
    install_empty_index();

    // A second file with a byte-identical preamble shares the PCH (the
    // key excludes the source path), but the preamble entry carries
    // file-local macro identities — its entries must stay scoped to the
    // file that built the blob.
    auto other_path = std::string(llvm::sys::path::parent_path(focus_path)) + "/other.cpp";
    auto other = session_store.open(project.file_table.intern(Spelling::absolute(other_path)));
    other->set_text(session->text().str());
    auto& other_entry = projections.entries[other->path_id];
    auto other_projection = std::make_shared<ASTProjection>();
    other_projection->pch_key = "key";
    other_entry.projection = std::move(other_projection);
    other_entry.current = true;

    auto locations = relations("macro", RelationKind::Reference);
    ZASSERT(locations.size() == 1);
    ZEXPECT(locations[0].path.ends_with("main.cpp"));
}

ZEST_CASE(DirtyPreambleServed) {
    add_focus("main.cpp", R"(#define §(macro)⟦FOO⟧ 1
int main() { return 0; }
)");
    open_with_overlay();
    install_empty_index();

    // Body edits dirty the session but never move preamble entries: as long
    // as the buffer still starts with the blob's preamble text, the
    // entry keeps serving — the prefix comparison is the freshness check.
    projections.entries[session->path_id].current = false;
    session->set_text(session->text().str() + "int more;\n");
    ZEXPECT(index_query.first_site(hash_of("FOO"), Fid{}, RelationKind::Definition));
}

ZEST_CASE(PreambleDriftSkipped) {
    add_focus("main.cpp", R"(#define §(macro)⟦FOO⟧ 1
int main() { return 0; }
)");
    open_with_overlay();
    install_empty_index();

    // A deferred PCH rebuild keeps an old blob while the buffer's
    // preamble moved on; once the buffer no longer starts with the blob's
    // stored preamble text, its entries must not be served.
    session->set_text("// drift\n" + session->text().str());
    ZEXPECT(!index_query.first_site(hash_of("FOO"), Fid{}, RelationKind::Definition).has_value());
}

ZEST_CASE(OverlayOutranksDisk) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
)");
    add_focus("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦foo⟧(); return 0; }
)");
    open_with_overlay();

    // Fabricate a divergent disk relation: another context's shard reports the
    // definition sits on line 0. The overlay (live context) must win.
    auto foo = hash_of("foo");
    index::FileIndex fake;
    index::Relation relation{
        .kind = RelationKind::Definition,
        .range = {0, 3}
    };
    relation.set_extent({0, 3});
    fake.relations[foo].push_back(relation);
    auto header_id = project.file_table.intern(Spelling::absolute(header_path("foo.h")));
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_shard(fake, {}, "xxx\n", os);
    project.index.shards[header_id] =
        index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
    project.index.edit(foo).reference_files.add(header_id.raw);

    auto def = index_query.first_site(foo, Fid{}, RelationKind::Definition);
    ZASSERT(def);
    ZEXPECT(line_of(*def) == 1u);
}

ZEST_CASE(SynthesizedContextSkipped) {
    // A header context's synthesized files name nothing on disk, and what
    // they declare is the host's to index: no entries, no dependency.
    auto synthesized = TestVFS::path(".clice-context.h");
    add_focus("main.cpp",
              std::format(R"(
#include "{}"
int main() {{ gen(); return 0; }}
)",
                          synthesized));
    prepare();
    params.input.add_synthesized({
        {synthesized, "inline void gen() {}\n"}
    });
    ZASSERT(try_compile());

    auto index = index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit)));
    ZASSERT(index.loaded());
    for(std::uint32_t i = 0; i < index.path_count(); i += 1) {
        ZEXPECT(index.path(i) != synthesized);
    }
    ZEXPECT(
        !llvm::any_of(unit->deps(), [&](const DepFile& dep) { return dep.path == synthesized; }));
}

ZEST_CASE(UnreadableBlobCleared) {
    dir.touch("junk.pch.idx", "not a flatbuffer");

    PCHState st;
    st.index_path = dir.path("junk.pch.idx");
    ZEXPECT(st.load_state() == nullptr);
    // The cleared path makes the pair look incomplete, so the next
    // ensure_pch round rebuilds it instead of retrying the mmap forever.
    ZEXPECT(st.index_path.empty());
}

};  // ZEST_SUITE(QueryOverlay)

}  // namespace
}  // namespace clice::testing
