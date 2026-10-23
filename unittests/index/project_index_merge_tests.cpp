module;

#include "support/prelude.h"
#ifndef _WIN32
#include <unistd.h>
#endif

module clice;

import :index.instantiation_shard;
import :index.project_index;
import :index.symbol;
import :index.tu_index;
import :project.project;
import :project.save;
import :sched.index.pending_table;
import :sched.index.pump;
import :unittests.utils.cdb_helper;
import :unittests.utils.index_internals;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils.tu_index_blob;
import :unittests.utils;
import :vfs.file_table;
import :vfs.ids;
import :vfs.path;

namespace clice::testing {
namespace {

/// Re-encode a TU index with its consumed-content hash column dropped,
/// as a file behind a PCM ships it.
std::string strip_content_hashes(llvm::StringRef data) {
    auto blob = tu_index_blob(index::TUIndex::from_bytes(data));
    blob.content_hashes.clear();
    return tu_index_bytes(blob);
}

ZEST_SUITE(ProjectIndexMerge) {

IndexerFixture fx;
kota::event_loop& loop = fx.loop;
Project& project = fx.project;
IndexPump& pump = fx.pump;

bool merge(llvm::StringRef data) {
    return fx.merge(data);
}

/// The symbols the index holds, saved or not.
std::uint32_t symbols() {
    std::uint32_t count = 0;
    project.index.for_each_symbol([&](index::SymbolHash, const index::SymbolView&, std::uint32_t) {
        count += 1;
        return true;
    });
    return count;
}

ZEST_CASE(MergeRejectsGarbage) {
    // A worker shipping corrupted bytes (torn write, stale format) must not
    // crash the master or leave partial state behind.
    ZASSERT(project.index.shard_count() == 0);
    ZASSERT(symbols() == 0u);

    std::string garbage = "definitely not a blob, but long enough to try";
    ZASSERT(!merge(garbage));

    ZASSERT(project.index.shard_count() == 0);
    ZASSERT(symbols() == 0u);
}

ZEST_CASE(SharedLocalPrecompiledInput) {
    // An internal symbol a unit names in a file it holds no entries of is
    // rejected, unless the unit reads that file through its precompiled
    // header or a module: their own units wrote its entries.
    TempDir tmp;
    tmp.touch("shared.h", "");
    tmp.touch("main.cpp", R"(#include "shared.h"
int use() { return 1; }
)");
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    auto blob = tu_index_blob(index::TUIndex::from_bytes(indexed.data));
    auto header_path = ranges::find_if(blob.paths, [](const std::string& path) {
        return llvm::StringRef(path).ends_with("shared.h");
    });
    ZASSERT(header_path != blob.paths.end());
    auto header_id = static_cast<std::uint32_t>(header_path - blob.paths.begin());
    auto focus_id = static_cast<std::uint32_t>(blob.paths.size() - 1);
    constexpr index::SymbolHash helper = ~0ull - 2;
    add_symbol(blob,
               helper,
               {.name = "helper", .scope = index::SymbolScope::Internal},
               {header_id, focus_id});
    auto planted = tu_index_bytes(blob);

    ZEXPECT(!merge(planted));

    auto header = project.file_table.intern(Spelling::absolute(*header_path));
    DepState input{.path_id = header, .version = project.file_table.intern_version(header, 1)};
    auto report = project.index.merge(planted, {input});
    ZASSERT(report);
    pump.apply_report(*report);
    auto focus = project.file_table.intern(Spelling::absolute(blob.paths.back()));
    llvm::SmallVector<Fid> reached;
    project.index.for_each_file_of_local(helper, header, [&](Fid file) {
        reached.push_back(file);
    });
    ZEXPECT(reached == llvm::SmallVector<Fid>{header, focus});

    // One the build saw gone still came out of the precompiled header.
    DepState gone{.path_id = header, .missing = true};
    ZEXPECT(project.index.merge(planted, {gone}).has_value());
}

ZEST_CASE(MergeIgnoresDiskDrift) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());

    merge(indexed.data);
    auto path_id = project.file_table.intern(Spelling::absolute(indexed.tu_path));
    auto* it = project.index.shard(path_id);
    ZASSERT(it);
    ZASSERT(it->content_hash() == llvm::xxh3_64bits("int value() { return 1; }\n"));

    // The disk moved on since the index was built. The blob is
    // self-contained — its entries pair with the generation it embeds, never
    // with the disk — so the re-merge is a pure variant hit and freshness
    // gating owns the drift.
    tmp.touch("main.cpp", "int renamed() { return 2; }\n");
    merge(indexed.data);
    ZASSERT(it->content_hash() == llvm::xxh3_64bits("int value() { return 1; }\n"));
    ZASSERT(it->variants().size() == std::size_t(1));
    ZASSERT(contributions_of(project.index, path_id).contains(path_id));

    // An index built from the new content opens a new generation.
    auto fresh = index_file(tmp, src);
    ZASSERT(!fresh.data.empty());
    merge(fresh.data);
    ZASSERT(it->content_hash() == llvm::xxh3_64bits("int renamed() { return 2; }\n"));
}

ZEST_CASE(MergeHitWritesNothing) {
    TempDir tmp;
    tmp.touch("main.cpp", "int steady() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, project);

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    merge(indexed.data);
    auto save_body = [&]() -> kota::task<> {
        co_await fx.async_save();
    };
    auto task = save_body();
    loop.schedule(task);
    loop.run();
    ZASSERT(project.index.pending_shard_writes() == 0u);

    // A re-merge whose variant the shard already stores is the steady state of
    // every background round: it must record contributions and touch no
    // blob at all.
    merge(indexed.data);
    ZASSERT(project.index.pending_shard_writes() == 0u);
}

ZEST_CASE(SharedHeaderVariants) {
    TempDir tmp;
    tmp.touch("shared.h",
              "#pragma once\n#ifdef MODE\nint mode_fn();\n#endif\n"
              "inline int shared_fn() { return 1; }\n");
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a() { return shared_fn(); }\n");
    tmp.touch("b.cpp", "#include \"shared.h\"\nint b() { return shared_fn(); }\n");

    auto a = index_file(tmp, tmp.path("a.cpp"));
    auto b = index_file(tmp, tmp.path("b.cpp"), {"-DMODE"});
    ZASSERT(!a.data.empty());
    ZASSERT(!b.data.empty());

    // Two TUs preprocess the header differently: both variants coexist in
    // one blob, each TU's contribution live.
    merge(a.data);
    merge(b.data);
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("shared.h")));
    auto& shard = *project.index.shard(header_id);
    ZASSERT(shard.variants().size() == std::size_t(2));
    ZASSERT(contributions_of(project.index, header_id).size() == std::size_t(2));

    // A third TU sharing a's preprocessing hits the stored variant: the
    // set does not grow, and neither existing contribution is disturbed.
    tmp.touch("c.cpp", "#include \"shared.h\"\nint c() { return shared_fn(); }\n");
    auto c = index_file(tmp, tmp.path("c.cpp"));
    ZASSERT(!c.data.empty());
    merge(c.data);
    ZASSERT(shard.variants().size() == std::size_t(2));
    ZASSERT(contributions_of(project.index, header_id).size() == std::size_t(3));

    // Re-indexing a TU whose header variant is unchanged must not disturb
    // the other TUs' variants either.
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a2() { return shared_fn(); }\n");
    auto fresh = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(!fresh.data.empty());
    merge(fresh.data);
    ZASSERT(shard.variants().size() == std::size_t(2));
    ZASSERT(contributions_of(project.index, header_id).size() == std::size_t(3));
}

ZEST_CASE(KnownVariantByHash) {
    TempDir tmp;
    tmp.touch("shared.h", "#pragma once\ninline int shared_fn() { return 1; }\n");
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a() { return shared_fn(); }\n");
    tmp.touch("b.cpp", "#include \"shared.h\"\nint b() { return shared_fn(); }\n");
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("shared.h")));
    auto header_section = [&](const index::TUIndex& view) {
        std::uint32_t found = view.section_count();
        for(std::uint32_t i = 0; i < view.section_count(); i += 1) {
            if(llvm::sys::path::filename(view.path(view.section_path(i))) == "shared.h") {
                found = i;
            }
        }
        ZASSERT(found < view.section_count());
        return found;
    };

    auto a = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(!a.data.empty());
    auto a_view = index::TUIndex::from_bytes(a.data);
    auto variant = a_view.section_hash(header_section(a_view));

    // A result naming a variant the index does not hold commits nothing:
    // the file must run again.
    auto early = index_file(tmp, tmp.path("b.cpp"), {}, {variant});
    auto outdated = project.index.merge(early.data, {});
    ZASSERT(!outdated);
    ZEXPECT(outdated.error() == index::MergeError::Outdated);
    auto b_id = project.file_table.intern(Spelling::absolute(early.tu_path));
    ZEXPECT(!project.index.indexed(b_id));

    // Once the index holds it, the bare hash counts as the file's
    // contribution like the bytes would.
    ZASSERT(merge(a.data));
    auto b = index_file(tmp, tmp.path("b.cpp"), {}, project.index.shard(header_id)->variants());
    auto b_view = index::TUIndex::from_bytes(b.data);
    ZASSERT(b_view.section_blob(header_section(b_view)).empty());
    ZASSERT(merge(b.data));
    ZEXPECT(project.index.indexed(b_id));
    ZEXPECT(project.index.shard(header_id)->variants().size() == std::size_t(1));
    ZEXPECT(contributions_of(project.index, header_id).size() == std::size_t(2));
}

ZEST_CASE(KnownVariantsFromManifest) {
    // Without a scanned include graph, the files a TU's last manifest names
    // still offer their stored variants.
    TempDir tmp;
    tmp.touch("shared.h", "#pragma once\ninline int shared_fn() { return 1; }\n");
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a() { return shared_fn(); }\n");
    auto a = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(merge(a.data));

    auto a_id = project.file_table.intern(Spelling::absolute(a.tu_path));
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("shared.h")));
    std::vector<std::uint64_t> expected = project.index.shard(header_id)->variants();
    expected.append_range(project.index.shard(a_id)->variants());
    ranges::sort(expected);
    ZEXPECT(project.index.known_variants(a_id, project.dep_graph.include_closure(a_id)) ==
            expected);

    auto other = project.file_table.intern(Spelling::absolute(tmp.path("other.cpp")));
    ZEXPECT(project.index.known_variants(other, project.dep_graph.include_closure(other)).empty());
}

ZEST_CASE(HeaderRegenerationReplaces) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");

    auto v1 = index_file(tmp, src);
    ZASSERT(!v1.data.empty());
    merge(v1.data);
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("dep.h")));
    auto tu_id = project.file_table.intern(Spelling::absolute(v1.tu_path));
    auto old_hash = contributions_of(project.index, header_id).lookup(tu_id);
    ZASSERT(old_hash != 0);

    // The header changes, a reindex captures it — and the header changes
    // AGAIN before the result merges. The worker's bytes are their own
    // generation: they merge verbatim regardless of the disk moving on, and
    // freshness gating owns the remaining drift.
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 2; }\n");
    auto v2 = index_file(tmp, src);
    ZASSERT(!v2.data.empty());
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 3; }\n");

    merge(v2.data);
    auto new_hash = contributions_of(project.index, header_id).lookup(tu_id);
    ZASSERT(new_hash != 0);
    ZASSERT(new_hash != old_hash);
    ZASSERT(project.index.shard(header_id)->has_variant(new_hash));
    // A new content generation never shares variant storage with the old one.
    ZASSERT(!project.index.shard(header_id)->has_variant(old_hash));
    ZASSERT(project.index.shard(header_id)->content_hash() ==
            llvm::xxh3_64bits("#pragma once\ninline int dep() { return 2; }\n"));
}

ZEST_CASE(RebuildRequeuesPinnedOwner) {
    TempDir tmp;
    tmp.touch("gen.h",
              "#pragma once\n#ifdef MODE\nint gen_mode();\n#endif\n"
              "inline int gen_fn() { return 1; }\n");
    tmp.touch("ga.cpp", "#include \"gen.h\"\nint ga() { return gen_fn(); }\n");
    tmp.touch("gb.cpp", "#include \"gen.h\"\nint gb() { return gen_fn(); }\n");

    auto a = index_file(tmp, tmp.path("ga.cpp"));
    auto b = index_file(tmp, tmp.path("gb.cpp"), {"-DMODE"});
    ZASSERT(!a.data.empty());
    ZASSERT(!b.data.empty());
    merge(a.data);
    merge(b.data);
    auto b_tu = project.file_table.intern(Spelling::absolute(b.tu_path));
    ZASSERT(!pump.pending_reason(b_tu).has_value());

    // The header moves to a new content generation and only ga catches up:
    // the rebuilt blob discards gb's variant. With no pending slot left for
    // gb, no in-process event would rebuild its variant — the rebuild itself
    // must re-enqueue it, and as ContentChanged: a reverted header reads
    // fresh by hash, which a deps-only slot would skip past.
    tmp.touch("gen.h",
              "#pragma once\n#ifdef MODE\nint gen_mode();\n#endif\n"
              "inline int gen_fn() { return 2; }\n");
    auto a2 = index_file(tmp, tmp.path("ga.cpp"));
    ZASSERT(!a2.data.empty());
    merge(a2.data);
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("gen.h")));
    ZASSERT(project.index.shard(header_id)->variants().size() == std::size_t(1));

    ZASSERT(pump.pending_reason(b_tu) == ReindexReason::ContentChanged);
    // ga's own fresh pin is stored: the rebuild must not re-enqueue it.
    ZASSERT(!pump.pending_reason(project.file_table.intern(Spelling::absolute(a2.tu_path)))
                 .has_value());
}

ZEST_CASE(RejectsCorruptSection) {
    TempDir tmp;
    tmp.touch("cor.h", "#pragma once\ninline int cor() { return 1; }\n");
    tmp.touch("cor_main.cpp", "#include \"cor.h\"\nint use_cor() { return cor(); }\n");

    auto indexed = index_file(tmp, tmp.path("cor_main.cpp"));
    ZASSERT(!indexed.data.empty());

    // Corrupt the focus file's blob bytes in place: the outer container still
    // verifies (sections are opaque bytes to it), only the blob's byte
    // identity check against the recorded section hash fails.
    std::string corrupt = indexed.data;
    auto tampered = index::TUIndex::from_bytes(corrupt);
    ZASSERT(tampered.loaded());
    auto blob = tampered.shard_of(tampered.path_count() - 1).bytes();
    ZASSERT(!blob.empty());
    auto pos = llvm::StringRef(corrupt).find(blob);
    ZASSERT(pos != llvm::StringRef::npos);
    for(std::size_t i = 0; i < blob.size(); i += 1) {
        corrupt[pos + i] = 'X';
    }

    // The header section verifies fine and is staged before the focus
    // section's identity check fails; the reject must discard the whole
    // result — a manifest whose recorded versions all match the disk would
    // otherwise be judged fresh forever with the focus file's shard missing.
    merge(corrupt);
    auto tu_id = project.file_table.intern(Spelling::absolute(indexed.tu_path));
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("cor.h")));
    ZASSERT(!project.index.indexed(tu_id));
    ZASSERT(!project.index.shard(header_id));
    // No global trace either: symbol identities from an untrusted result
    // would stay canonical for their hashes forever (later merges only
    // fill empty names), and stray FileVersions would persist with the
    // next save.
    ZASSERT(symbols() == 0u);
    ZASSERT(project.file_table.versions.empty());

    // The intact result still merges afterwards.
    merge(indexed.data);
    ZASSERT(project.index.indexed(tu_id));
    ZASSERT(project.index.shard(header_id) != nullptr);
}

ZEST_CASE(HashlessRemergeHits) {
    TempDir tmp;
    tmp.touch("pcm.cpp", "int hashless_fn() { return 7; }\n");
    auto src = tmp.path("pcm.cpp");
    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());

    // A file behind a PCM ships no consumed-content hash; the variant
    // identity is the blob's own byte hash, so membership needs no
    // content confirmation at all.
    auto stripped = strip_content_hashes(indexed.data);
    ZASSERT(!stripped.empty());

    merge(stripped);
    auto path_id = project.file_table.intern(Spelling::absolute(src));
    ZASSERT(project.index.shard(path_id)->variants().size() == std::size_t(1));

    // Re-merging the same variant must register as a hit, not append the
    // stored variant to the blob a second time.
    merge(stripped);
    ZASSERT(project.index.shard(path_id)->variants().size() == std::size_t(1));
}

ZEST_CASE(DropPendsUnitNames) {
    TempDir tmp;
    tmp.touch("main.cpp",
              "static int lonely_helper() { return 1; }\n"
              "int main() { return lonely_helper(); }\n");
    open_store(tmp, project);
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    merge(indexed.data);
    fx.save(index::SearchRebuild::Idle);
    ZASSERT(project.index.search_index().loaded());
    auto file = project.file_table.intern(Spelling::absolute(indexed.tu_path));
    ZASSERT(!project.index.search_pending_files().contains(file));

    // The built index still lists the static of the variant the drop
    // retires: searches must read the shard instead.
    fx.drop_index(file);
    ZEXPECT(project.index.search_pending_files().contains(file));
}

ZEST_CASE(IncludeResolutionsPerUnit) {
    // Two units resolve one `#include` of a shared header to different
    // files: the header's edges name both, and a unit leaving takes only
    // its own.
    TempDir tmp;
    tmp.touch("shared.h", "#pragma once\n#include \"config.h\"\nint shared_value();\n");
    tmp.touch("a/config.h", "#pragma once\n");
    tmp.touch("b/config.h", "#pragma once\n");
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a() { return 1; }\n");
    tmp.touch("b.cpp", "#include \"shared.h\"\nint b() { return 2; }\n");
    auto a = index_file(tmp, tmp.path("a.cpp"), {"-I" + tmp.path("a")});
    auto b = index_file(tmp, tmp.path("b.cpp"), {"-I" + tmp.path("b")});
    ZASSERT(!a.data.empty());
    ZASSERT(!b.data.empty());
    ZASSERT(merge(a.data));
    ZASSERT(merge(b.data));

    auto shared = project.file_table.intern(Spelling::absolute(tmp.path("shared.h")));
    auto targets = [&] {
        std::vector<std::string> found;
        auto edges = project.index.include_edges(shared);
        for(auto& edge: edges) {
            // One directive, resolved two ways.
            ZEXPECT(edge.line == edges.front().line);
            found.push_back(
                llvm::sys::path::filename(llvm::sys::path::parent_path(edge.target)).str());
        }
        ranges::sort(found);
        return found;
    };
    ZEXPECT(targets() == std::vector<std::string>{"a", "b"});

    fx.drop_index(project.file_table.intern(Spelling::absolute(a.tu_path)));
    ZEXPECT(targets() == std::vector<std::string>{"b"});
}

};  // ZEST_SUITE(ProjectIndexMerge)

ZEST_SUITE(ProjectIndexFreshness) {

/// A merged TU with one header dependency, ready for staleness probing.
struct Indexed {
    TempDir tmp;
    IndexerFixture f;
    std::string src;
    std::string header;

    bool setup() {
        tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
        tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
        src = tmp.path("main.cpp");
        header = tmp.path("dep.h");
        // Age the files out of the mtime guard window so their stats can
        // confirm them, the way real project files predate an index run.
        if(!set_file_mtime(src, file_mtime_ns(src) - 10'000'000'000) ||
           !set_file_mtime(header, file_mtime_ns(header) - 10'000'000'000)) {
            return false;
        }
        auto indexed = index_file(tmp, src);
        if(indexed.data.empty()) {
            return false;
        }
        f.merge(indexed.data);
        return true;
    }
};

ZEST_CASE(CreatedHeaderStales) {
    // Where a failed include looked is an input of the TU: a header
    // appearing there makes its index stale.
    TempDir tmp;
    tmp.touch("main.cpp", "#include \"gen.h\"\nint use() { return 0; }\n");
    auto src = tmp.path("main.cpp");
    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    IndexerFixture f;
    f.merge(indexed.data);
    auto tu = f.project.file_table.intern(Spelling::absolute(indexed.tu_path));
    auto header = f.project.file_table.intern(Spelling::absolute(tmp.path("gen.h")));
    llvm::SmallVector<Fid> lookups;
    f.project.index.for_each_absent_lookup(header, [&](Fid unit) { lookups.push_back(unit); });
    ZASSERT(lookups == llvm::SmallVector<Fid>{tu});
    ZASSERT(f.project.file_table.disk.seen_missing(header));
    ZASSERT(!f.needs_reindex(src));

    tmp.touch("gen.h", "int make();\n");
    f.begin_round();
    ZASSERT(f.needs_reindex(src));
}

#ifndef _WIN32
ZEST_CASE(AbsentSpellingsOnePlace) {
    // A failed include looked in a directory under two spellings (one a
    // symlink): one place, recorded once, and a reindex drops it cleanly.
    TempDir tmp;
    tmp.touch("main.cpp", "#include \"gen.h\"\nint use() { return 0; }\n");
    tmp.mkdir("real");
    ZASSERT(::symlink(tmp.path("real").c_str(), tmp.path("link").c_str()) == 0);
    auto src = tmp.path("main.cpp");
    auto indexed = index_file(tmp, src, {"-I" + tmp.path("real"), "-I" + tmp.path("link")});
    ZASSERT(!indexed.data.empty());
    IndexerFixture f;
    f.merge(indexed.data);
    f.merge(indexed.data);
    auto tu = f.project.file_table.intern(Spelling::absolute(indexed.tu_path));
    auto manifest = *f.project.index.manifest(tu);
    auto place = f.project.file_table.intern(Spelling::absolute(tmp.path("real/gen.h")));
    ZASSERT(ranges::count_if(manifest.absent, [&](VersionID version) {
                return f.project.file_table.version(version).fid == place;
            }) == 1);
}
#endif

ZEST_CASE(TouchStaysFresh) {
    Indexed x;
    open_store(x.tmp, x.f.project);
    ZASSERT(x.setup());
    x.f.save();
    ZASSERT(!has_unsaved_state(x.f.project));
    ZASSERT(!x.f.needs_reindex(x.src));

    // Same bytes, new mtime: the stat fast path misses, the hash proves a
    // mere touch, and nothing persisted needs rewriting.
    ZASSERT(set_file_mtime(x.header, file_mtime_ns(x.header) + 5'000'000'000));
    x.f.begin_round();
    ZASSERT(!x.f.needs_reindex(x.src));
    ZASSERT(!has_unsaved_state(x.f.project));
}

ZEST_CASE(PreservedMtimeEditStale) {
    Indexed x;
    ZASSERT(x.setup());
    auto recorded = file_mtime_ns(x.header);

    // Different content restored to the recorded mtime (rsync -t, git
    // restore-mtime): equality of the stat is not enough — the size moved,
    // and the hash check must catch the edit.
    x.tmp.touch("dep.h", "#pragma once\ninline int dep() { return 12345; }\n");
    ZASSERT(set_file_mtime(x.header, recorded));
    x.f.begin_round();
    ZASSERT(x.f.needs_reindex(x.src));
}

ZEST_CASE(AllDepsChecked) {
    TempDir tmp;
    tmp.touch("first.h", "#pragma once\ninline int first() { return 1; }\n");
    tmp.touch("second.h", "#pragma once\ninline int second() { return 2; }\n");
    tmp.touch("main.cpp",
              "#include \"first.h\"\n#include \"second.h\"\n"
              "int use() { return first() + second(); }\n");
    IndexerFixture f;
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    f.merge(indexed.data);
    ZASSERT(!f.needs_reindex(tmp.path("main.cpp")));

    // Only the second dependency changes; a partial iteration would call
    // the TU fresh.
    auto recorded = file_mtime_ns(tmp.path("second.h"));
    tmp.touch("second.h", "#pragma once\ninline int second() { return 22222; }\n");
    ZASSERT(set_file_mtime(tmp.path("second.h"), recorded));
    f.begin_round();
    ZASSERT(f.needs_reindex(tmp.path("main.cpp")));
}

};  // ZEST_SUITE(ProjectIndexFreshness)

ZEST_SUITE(ProjectIndexInstantiations) {

// box.h's template instantiated by units each with their own argument.
TempDir tmp;
IndexerFixture f;

ProjectIndexInstantiationsTEST() {
    tmp.touch("box.h", R"(#pragma once
template <typename T>
struct Box {
    T value;
    void show() { value.print(); }
};
)");
    tmp.touch("a.h", R"(#pragma once
struct A { void print(); };
)");
    tmp.touch("b.h", R"(#pragma once
struct B { void print(); };
)");
    tmp.touch("a.cpp", R"(#include "box.h"
#include "a.h"
void a() { Box<A>{}.show(); }
)");
    tmp.touch("a2.cpp", R"(#include "box.h"
#include "a.h"
void a2() { Box<A>{}.show(); }
)");
    tmp.touch("b.cpp", R"(#include "box.h"
#include "b.h"
void b() { Box<B>{}.show(); }
)");
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("a.cpp"),  {}},
                  {tmp.root, tmp.path("a2.cpp"), {}},
                  {tmp.root, tmp.path("b.cpp"),  {}},
    }));
    scan_all(f.project.cdb, f.project.toolchain, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();
    open_store(tmp, f.project);
}

Fid id(llvm::StringRef name) {
    return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
}

/// Index a unit, skipping what the index offers it, and merge it.
bool index(llvm::StringRef name) {
    f.begin_round();
    auto known = f.known_instantiations(id(name));
    auto indexed = index_file(tmp, tmp.path(name), {}, {}, known);
    return !indexed.data.empty() && f.merge(indexed.data);
}

const index::InstantiationShard* box() {
    return f.project.index.instantiation_shard(id("box.h"));
}

/// The live records naming `print` in box.h: one per argument some unit
/// instantiates the template with.
std::size_t prints() {
    std::size_t count = 0;
    box()->for_each_record([&](const index::InstantiationRecord& record) {
        count += record.kind == index::InstantiationKind::Reference ? 1 : 0;
        return true;
    });
    return count;
}

ZEST_CASE(ResolutionsSharedAndSkipped) {
    ZASSERT(index("a.cpp"));
    ZASSERT(box());
    ZEXPECT(box()->resolutions().size() == 1u);
    ZEXPECT(prints() == 1u);

    // a2.cpp skips Box<A>::show, which a.cpp walked, and uses its resolution.
    f.begin_round();
    auto known = f.known_instantiations(id("a2.cpp"));
    ZEXPECT(!known.empty());
    ZASSERT(index("a2.cpp"));
    auto walked = f.project.index.manifest(id("a.cpp"))->instantiations;
    auto skipped = f.project.index.manifest(id("a2.cpp"))->instantiations;
    ZASSERT(skipped.size() == 1u);
    ZEXPECT(skipped == walked);

    ZASSERT(index("b.cpp"));
    ZEXPECT(box()->resolutions().size() == 2u);
    ZEXPECT(prints() == 2u);
}

ZEST_CASE(ResolutionLivesWhileUsed) {
    ZASSERT(index("a.cpp"));
    ZASSERT(index("a2.cpp"));
    ZASSERT(index("b.cpp"));

    // a2.cpp still uses Box<A>'s resolution when its producer leaves.
    f.drop_index(id("a.cpp"));
    ZEXPECT(prints() == 2u);
    // Without a current run behind it the key is offered no more.
    f.begin_round();
    auto key = f.known_instantiations(id("b.cpp"));
    ZEXPECT(key.empty());

    f.drop_index(id("a2.cpp"));
    ZEXPECT(prints() == 1u);
    f.save();
    ZEXPECT(box()->resolutions().size() == 1u);
}

ZEST_CASE(SkippedKeysFormUse) {
    // A unit skipping every key it meets walks nothing new, yet the
    // resolutions it uses together form a set no unit used before: the
    // file's blob must store it, or the manifest names a use nothing holds.
    tmp.touch("a2.cpp", R"(#include "box.h"
#include "a.h"
#include "b.h"
void a2() { Box<A>{}.show(); Box<B>{}.show(); }
)");
    ZASSERT(index("a.cpp"));
    ZASSERT(index("b.cpp"));
    ZASSERT(index("a2.cpp"));
    auto uses = f.project.index.manifest(id("a2.cpp"))->instantiations;
    ZASSERT(uses.size() == 1u);
    ZEXPECT(box()->has_use(uses.front().use));

    // a2.cpp alone keeps both resolutions alive.
    f.drop_index(id("a.cpp"));
    f.drop_index(id("b.cpp"));
    f.save();
    ZEXPECT(prints() == 2u);
}

ZEST_CASE(StaleProducerNotOffered) {
    ZASSERT(index("a.cpp"));
    f.begin_round();
    ZEXPECT(!f.known_instantiations(id("a2.cpp")).empty());
    // Its own run would confirm the key, not skip it.
    ZEXPECT(f.known_instantiations(id("a.cpp")).empty());
    // An open box.h stands in for its stored instantiations: it walks them.
    ZEXPECT(f.known_instantiations(id("box.h")).empty());

    // An edit of what the instantiation read puts its producer behind the
    // disk, as the change event reaching the invalidator marks it.
    tmp.touch("a.h", R"(#pragma once
struct A { void print(); void print(int); };
)");
    f.project.index.mark_behind(id("a.cpp"));
    f.begin_round();
    ZEXPECT(f.known_instantiations(id("a2.cpp")).empty());
}

ZEST_CASE(LaterRunCatchesUp) {
    // A run started before its unit fell behind the disk read the old file:
    // its merge leaves the unit behind. One started after catches it up.
    ZASSERT(index("a.cpp"));
    f.begin_round();
    auto early = index_file(tmp, tmp.path("a.cpp"), {}, {}, f.known_instantiations(id("a.cpp")));
    ZASSERT(!early.data.empty());
    f.project.index.mark_behind(id("a.cpp"));
    ZASSERT(f.merge(early.data));
    f.begin_round();
    ZEXPECT(f.known_instantiations(id("a2.cpp")).empty());

    IndexInternals::fell_behind_at(f.project.index, id("a.cpp"), 0);
    ZASSERT(index("a.cpp"));
    f.begin_round();
    ZEXPECT(!f.known_instantiations(id("a2.cpp")).empty());
}

ZEST_CASE(ReplacedRunNotOffered) {
    ZASSERT(index("a.cpp"));
    // a.cpp's next run instantiates Box<A> no more: the key's run is gone.
    tmp.touch("a.cpp", R"(#include "box.h"
#include "a.h"
void a() {}
)");
    ZASSERT(index("a.cpp"));
    f.begin_round();
    ZEXPECT(f.known_instantiations(id("a2.cpp")).empty());
}

ZEST_CASE(GoneRunKeysCompacted) {
    // Box<A>::show resolves nothing: box.h holds its key alone.
    tmp.touch("box.h", R"(#pragma once
template <typename T>
struct Box {
    void show() {}
};
)");
    ZASSERT(index("a.cpp"));
    ZASSERT(box());
    ZEXPECT(box()->resolutions().empty());
    ZEXPECT(box()->keys().size() == 1u);

    // a.cpp's next run instantiates Box<A> no more: nothing of box.h's
    // instantiation shard holds, and the save retires it.
    tmp.touch("a.cpp", R"(#include "box.h"
#include "a.h"
void a() {}
)");
    ZASSERT(index("a.cpp"));
    f.save();
    ZEXPECT(!box());
}

ZEST_CASE(HeaderInternalUnkeyed) {
    // A type of a.h's anonymous namespace: what Box<A>::show resolves names
    // it outside box.h, which only a unit walking the instantiation lists
    // among the files naming `print`. No key is offered for it, so when
    // a.cpp stops instantiating, a2.cpp still leads from a.h to the record.
    tmp.touch("a.h", R"(#pragma once
namespace {
struct A { void print(); };
}
)");
    ZASSERT(index("a.cpp"));
    f.begin_round();
    ZEXPECT(f.known_instantiations(id("a2.cpp")).empty());
    ZASSERT(index("a2.cpp"));
    tmp.touch("a.cpp", R"(#include "box.h"
#include "a.h"
void a() {}
)");
    ZASSERT(index("a.cpp"));

    auto* header = f.project.index.shard(id("a.h"));
    ZASSERT(header);
    std::optional<index::SymbolHash> print;
    header->for_each_local_symbol([&](index::SymbolHash hash, const index::SymbolView& identity) {
        if(identity.name == "print") {
            print = hash;
        }
        return true;
    });
    ZASSERT(print);
    llvm::SmallVector<Fid> reached;
    f.project.index.for_each_file_of_local(*print, id("a.h"), [&](Fid file) {
        reached.push_back(file);
    });
    ZEXPECT(ranges::contains(reached, id("box.h")));
}

ZEST_CASE(GoneKeyReruns) {
    ZASSERT(index("a.cpp"));
    f.begin_round();
    auto known = f.known_instantiations(id("a2.cpp"));
    ZASSERT(!known.empty());
    // The key leaves before a2.cpp's run that skipped it merges.
    f.drop_index(id("a.cpp"));
    f.save();
    auto indexed = index_file(tmp, tmp.path("a2.cpp"), {}, {}, known);
    auto merged = f.project.index.merge(indexed.data, {});
    ZASSERT(!merged);
    ZEXPECT(merged.error() == index::MergeError::Outdated);
}

};  // ZEST_SUITE(ProjectIndexInstantiations)

}  // namespace
}  // namespace clice::testing
