#include <algorithm>
#include <filesystem>
#include <optional>
#include <tuple>

#include "test/temp_dir.h"
#include "test/test.h"
#include "test/tester.h"
#include "index/merged_index.h"
#include "index/serialization.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {

namespace {

TEST_SUITE(MergedIndex, Tester) {

index::TUIndex tu_index;

void build_index(llvm::StringRef code,
                 std::source_location location = std::source_location::current()) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile());

    tu_index = index::TUIndex::build(*unit);
};

void EXPECT_SELECT(llvm::StringRef pos,
                   llvm::StringRef expect_range,
                   llvm::StringRef file = "",
                   std::source_location location = std::source_location::current()) {
    auto offset = point(pos, file);
    auto expected = range(expect_range, file);

    auto fid = file.empty() ? unit->interested_file() : unit->file_id(file);
    auto& index = tu_index.file_indices[fid];

    auto it =
        std::ranges::lower_bound(index.occurrences, offset, {}, [](index::Occurrence& occurrence) {
            return occurrence.range.end;
        });

    auto err = std::format("Fail to find symbol for offset: {}, expected range: {}",
                           offset,
                           dump(expected));

    ASSERT_TRUE(it != index.occurrences.end());

    /// FIXME: Make eq pretty print reflectable struct.
    ASSERT_EQ(dump(it->range), dump(expected));
}

TEST_CASE(Serialization) {
    build_index(R"(
            struct Foo { int x; int y; };
            Foo make_foo() { return Foo{1, 2}; }
            int use_foo() { return make_foo().x; }
        )");

    llvm::StringMap<index::MergedIndex> merged_indices;
    auto& graph = tu_index.graph;
    for(auto& [fid, index]: tu_index.file_indices) {
        llvm::StringRef path = graph.paths[graph.path_id(fid)];
        merged_indices[path].merge("tu0", graph.include_location_id(fid), index, {});
    }

    for(auto& [path, merged]: merged_indices) {
        llvm::SmallString<1024> s;
        llvm::raw_svector_ostream os(s);

        merged.serialize(os);

        auto view = index::MergedIndex(s);
        ASSERT_TRUE(merged == view);
    }
}

TEST_CASE(RevisionAndFlipBack) {
    build_index(R"(
            int flip_func() { return 1; }
        )");

    index::MergedIndex merged;
    ASSERT_EQ(merged.revision(), 0u);

    auto fid = unit->interested_file();
    merged.merge("tu0", tu_index.graph.include_location_id(fid), tu_index.main_file_index, {});
    auto merged_rev = merged.revision();
    ASSERT_TRUE(merged_rev != 0u);
    ASSERT_TRUE(merged.need_rewrite());

    // The flip save() performs after a commit: the serialized twin is
    // buffer-backed (no heap Impl, not dirty) and answers identically.
    llvm::SmallString<1024> s;
    llvm::raw_svector_ostream os(s);
    merged.serialize(os);
    auto reloaded = index::MergedIndex(s);
    ASSERT_FALSE(reloaded.need_rewrite());
    ASSERT_EQ(reloaded.revision(), 0u);

    // Every mutation bumps the revision, so a save can prove no merge
    // landed across its commit await. (Ordering is load-bearing: operator==
    // materializes both sides' Impl, and serialize() compacts removed rows
    // and caches — the comparison is only valid before remove()/lookup()
    // touch either side.)
    ASSERT_TRUE(merged == reloaded);
    merged.remove("tu0");
    ASSERT_TRUE(merged.revision() != merged_rev && merged.revision() != 0u);
}

TEST_CASE(LookupByOffset) {
    build_index(R"(
            int §(func)⟦§(func)foo⟧() { return 42; }
            int bar() { return §(ref)⟦§(ref)foo⟧(); }
        )");

    // Merge the main file index into a MergedIndex.
    index::MergedIndex merged;
    auto fid = unit->interested_file();
    merged.merge("tu0", tu_index.graph.include_location_id(fid), tu_index.main_file_index, {});

    // Lookup at the reference offset should find an occurrence.
    auto ref_offset = point("ref");
    bool found = false;
    merged.lookup(ref_offset, [&](const index::Occurrence& occ) {
        if(occ.range.contains(ref_offset)) {
            found = true;
        }
        return true;
    });
    ASSERT_TRUE(found);
}

TEST_CASE(LookupBySymbolAndKind) {
    build_index(R"(
            void §(target)target_func() {}
            void caller() { §(call)target_func(); }
        )");

    index::MergedIndex merged;
    auto fid = unit->interested_file();
    merged.merge("tu0", tu_index.graph.include_location_id(fid), tu_index.main_file_index, {});

    // Find the target_func symbol hash via occurrence lookup.
    auto target_offset = point("target");
    index::SymbolHash target_hash = 0;
    merged.lookup(target_offset, [&](const index::Occurrence& occ) {
        if(occ.range.contains(target_offset)) {
            target_hash = occ.target;
            return false;
        }
        return true;
    });
    ASSERT_TRUE(target_hash != 0);

    // Lookup Definition relation for the symbol.
    bool found_def = false;
    merged.lookup(target_hash, RelationKind::Definition, [&](const index::Relation& rel) {
        found_def = true;
        return true;
    });
    ASSERT_TRUE(found_def);
}

TEST_CASE(MultipleMergesDedup) {
    add_file("header.h", R"(
            #pragma once
            inline int shared() { return 1; }
        )");
    add_main("a.cpp", R"(
            #include "header.h"
            int use_a() { return shared(); }
        )");
    ASSERT_TRUE(compile());
    auto tu_a = index::TUIndex::build(*unit);

    add_file("header.h", R"(
            #pragma once
            inline int shared() { return 1; }
        )");
    add_main("b.cpp", R"(
            #include "header.h"
            int use_b() { return shared(); }
        )");
    ASSERT_TRUE(compile());
    auto tu_b = index::TUIndex::build(*unit);

    // Merge header indices from both TUs into same MergedIndex.
    index::MergedIndex merged_header;
    for(auto& [fid, file_index]: tu_a.file_indices) {
        merged_header.merge("tu0", tu_a.graph.include_location_id(fid), file_index, {});
    }
    for(auto& [fid, file_index]: tu_b.file_indices) {
        merged_header.merge("tu1", tu_b.graph.include_location_id(fid), file_index, {});
    }

    // Serialize and deserialize to verify dedup survives round-trip.
    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    merged_header.serialize(os);

    auto restored = index::MergedIndex(buf);
    ASSERT_TRUE(merged_header == restored);
}

TEST_CASE(SerializationRoundTripInMemory) {
    build_index(R"(
            struct Foo { int x; };
            Foo make() { return Foo{42}; }
        )");

    // Merge using the include_id overload (same as existing Serialization test).
    index::MergedIndex merged;
    auto fid = unit->interested_file();
    auto include_id = tu_index.graph.include_location_id(fid);
    merged.merge("tu0", include_id, tu_index.main_file_index, {});

    // Serialize.
    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    merged.serialize(os);

    // Deserialize and compare.
    auto restored = index::MergedIndex(buf);
    ASSERT_TRUE(merged == restored);

    // Lookup should work on the deserialized version too.
    bool found = false;
    for(auto& occ: tu_index.main_file_index.occurrences) {
        restored.lookup(occ.range.begin, [&](const index::Occurrence& o) {
            if(o.range.begin == occ.range.begin) {
                found = true;
            }
            return true;
        });
        if(found)
            break;
    }
    ASSERT_TRUE(found);
}

TEST_CASE(RemoveCompilationContext) {
    build_index(R"(
            int foo() { return 42; }
            int bar() { return foo(); }
        )");

    // Merge as a compilation context (using the build_at overload).
    index::MergedIndex merged;
    auto fid = unit->interested_file();
    merged.merge("tu0", tu_index.built_at, {}, tu_index.main_file_index, {});

    // Verify occurrence lookup works before remove.
    bool found_before = false;
    for(auto& occ: tu_index.main_file_index.occurrences) {
        merged.lookup(occ.range.begin, [&](const index::Occurrence& o) {
            found_before = true;
            return false;
        });
        if(found_before)
            break;
    }
    ASSERT_TRUE(found_before);

    // Remove the compilation context.
    merged.remove("tu0");

    // Serialize and verify the removed data round-trips.
    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    merged.serialize(os);
    // Should not crash.
    auto restored = index::MergedIndex(buf);
}

TEST_CASE(RemoveHeaderContext) {
    add_file("header.h", R"(
            #pragma once
            inline int shared() { return 1; }
        )");
    add_main("main.cpp", R"(
            #include "header.h"
            int use() { return shared(); }
        )");
    ASSERT_TRUE(compile());
    tu_index = index::TUIndex::build(*unit);

    // Merge header index as header context.
    index::MergedIndex merged_header;
    for(auto& [fid, file_index]: tu_index.file_indices) {
        merged_header.merge("tu0", tu_index.graph.include_location_id(fid), file_index, {});
    }

    // Remove should not crash.
    merged_header.remove("tu0");

    // Serialize after remove should work.
    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    merged_header.serialize(os);
}

TEST_CASE(RemergeReplacesContribution) {
    add_file("header.h", R"(
            #pragma once
            inline int shared() { return 1; }
        )");
    add_main("main.cpp", R"(
            #include "header.h"
            int use() { return shared(); }
        )");
    ASSERT_TRUE(compile());
    tu_index = index::TUIndex::build(*unit);

    auto header_fid = unit->file_id("header.h");
    auto& header_idx = tu_index.file_indices[header_fid];
    auto include_id = tu_index.graph.include_location_id(header_fid);

    // The symbol defined in the header: its Definition relation exists only
    // in the header's file index, not in main's (which only references it).
    index::SymbolHash defined{};
    for(auto& [symbol, relations]: header_idx.relations) {
        for(auto& relation: relations) {
            if(RelationKind(relation.kind) & RelationKind(RelationKind::Definition)) {
                defined = symbol;
            }
        }
    }

    auto has_definition = [&](index::MergedIndex& merged) {
        bool found = false;
        merged.lookup(defined, RelationKind::Definition, [&](const index::Relation&) {
            found = true;
            return false;
        });
        return found;
    };

    index::MergedIndex merged;
    merged.merge("tu0", include_id, header_idx, {});
    ASSERT_TRUE(has_definition(merged));

    // Identical re-merge (a touch): the contribution is resurrected, not lost.
    merged.merge("tu0", include_id, header_idx, {});
    ASSERT_TRUE(has_definition(merged));

    // Re-merge of the same TU with different content: the old contribution
    // is masked instead of being served alongside the new one.
    merged.merge("tu0", include_id, tu_index.main_file_index, {});
    ASSERT_FALSE(has_definition(merged));
}

TEST_CASE(RemergePreservesOtherTus) {
    add_file("header.h", R"(
            #pragma once
            inline int shared() { return 1; }
        )");
    add_main("main.cpp", R"(
            #include "header.h"
            int use() { return shared(); }
        )");
    ASSERT_TRUE(compile());
    tu_index = index::TUIndex::build(*unit);

    auto header_fid = unit->file_id("header.h");
    auto& header_idx = tu_index.file_indices[header_fid];
    auto include_id = tu_index.graph.include_location_id(header_fid);

    index::SymbolHash defined{};
    for(auto& [symbol, relations]: header_idx.relations) {
        for(auto& relation: relations) {
            if(RelationKind(relation.kind) & RelationKind(RelationKind::Definition)) {
                defined = symbol;
            }
        }
    }

    index::MergedIndex merged;
    merged.merge("tu0", include_id, header_idx, {});
    merged.merge("tu1", include_id, header_idx, {});

    // TU 0 moves on, TU 1 still holds the shared canonical contribution.
    merged.merge("tu0", include_id, tu_index.main_file_index, {});

    bool found = false;
    merged.lookup(defined, RelationKind::Definition, [&](const index::Relation&) {
        found = true;
        return false;
    });
    ASSERT_TRUE(found);
}

TEST_CASE(CompactionDropsMasked) {
    build_index(R"(
            int §(target)foo() { return 42; }
        )");

    // Merge as compilation context, then remove: the rows are masked.
    index::MergedIndex merged;
    merged.merge("tu0", tu_index.built_at, {}, tu_index.main_file_index, {});
    merged.remove("tu0");

    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    merged.serialize(os);

    // Serialized shards are served through buffer-only lookups that never
    // consult the removed bitmap — masked rows must not reach disk at all.
    auto restored = index::MergedIndex(buf);
    auto offset = point("target");
    bool found = false;
    restored.lookup(offset, [&](const index::Occurrence&) {
        found = true;
        return false;
    });
    ASSERT_FALSE(found);
}

TEST_CASE(SerializeCompactsInPlace) {
    // Two contributions with distinct content, so each gets its own
    // canonical id; only one is removed.
    index::FileIndex live_idx;
    live_idx.occurrences.emplace_back(index::Range{0, 3}, 100);
    index::FileIndex dead_idx;
    dead_idx.occurrences.emplace_back(index::Range{10, 13}, 200);

    index::MergedIndex merged;
    merged.merge("tu0", std::uint32_t(0), live_idx, "synthetic");
    merged.merge("tu1", std::uint32_t(0), dead_idx, "synthetic");
    merged.remove("tu1");

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    merged.serialize(os);

    // The save flip is conditional: when it does not happen, the in-memory
    // impl — now compacted by serialize() — keeps serving queries. Surviving
    // rows must still resolve and removed ones stay gone.
    auto hits_at = [&](std::uint32_t offset) {
        std::size_t hits = 0;
        merged.lookup(offset, [&](const index::Occurrence&) {
            hits += 1;
            return true;
        });
        return hits;
    };
    ASSERT_EQ(hits_at(1), 1u);
    ASSERT_EQ(hits_at(11), 0u);
    ASSERT_TRUE(merged.has_contribution("tu0"));
    ASSERT_FALSE(merged.has_contribution("tu1"));

    // A second serialize of the compacted impl round-trips identically.
    llvm::SmallString<1024> again;
    llvm::raw_svector_ostream os2(again);
    merged.serialize(os2);
    ASSERT_EQ(llvm::StringRef(buf), llvm::StringRef(again));
}

TEST_CASE(HasContributionTracking) {
    add_file("header.h", R"(
            #pragma once
            inline int shared() { return 1; }
        )");
    add_main("main.cpp", R"(
            #include "header.h"
            int use() { return shared(); }
        )");
    ASSERT_TRUE(compile());
    tu_index = index::TUIndex::build(*unit);

    auto header_fid = unit->file_id("header.h");
    auto& header_idx = tu_index.file_indices[header_fid];
    auto include_id = tu_index.graph.include_location_id(header_fid);

    index::MergedIndex merged;
    merged.merge("tu0", include_id, header_idx, {});
    merged.merge("tu1", include_id, header_idx, {});

    ASSERT_TRUE(merged.has_contribution("tu0"));
    ASSERT_TRUE(merged.has_contribution("tu1"));
    ASSERT_FALSE(merged.has_contribution("tu2"));

    // The buffer path must answer without deserializing the shard.
    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    merged.serialize(os);
    auto restored = index::MergedIndex(buf);
    ASSERT_TRUE(restored.has_contribution("tu0"));
    ASSERT_FALSE(restored.has_contribution("tu2"));

    merged.remove("tu0");
    ASSERT_FALSE(merged.has_contribution("tu0"));
    ASSERT_TRUE(merged.has_contribution("tu1"));
}

TEST_CASE(LookupFiltersRemoved) {
    build_index(R"(
            int §(target)foo() { return 42; }
        )");

    // Merge as compilation context.
    index::MergedIndex merged;
    merged.merge("tu0", tu_index.built_at, {}, tu_index.main_file_index, {});

    // Verify lookup finds something before removal.
    auto offset = point("target");
    bool found_before = false;
    merged.lookup(offset, [&](const index::Occurrence& occ) {
        if(occ.range.contains(offset))
            found_before = true;
        return true;
    });
    ASSERT_TRUE(found_before);

    // Remove the compilation context.
    merged.remove("tu0");

    // Verify lookup finds nothing after removal.
    bool found_after = false;
    merged.lookup(offset, [&](const index::Occurrence& occ) {
        if(occ.range.contains(offset))
            found_after = true;
        return true;
    });
    ASSERT_FALSE(found_after);
}

TEST_CASE(CacheInvalidatedAfterMerge) {
    build_index(R"(
            int §(first)foo() { return 42; }
        )");

    // Merge first TU as header context.
    index::MergedIndex merged;
    auto fid = unit->interested_file();
    merged.merge("tu0", tu_index.graph.include_location_id(fid), tu_index.main_file_index, {});

    // Trigger cache build by doing a lookup.
    auto first_offset = point("first");
    bool found_first = false;
    merged.lookup(first_offset, [&](const index::Occurrence& occ) {
        if(occ.range.contains(first_offset))
            found_first = true;
        return true;
    });
    ASSERT_TRUE(found_first);

    // Build a second TU with different content.
    build_index(R"(
            int §(second)bar() { return 99; }
        )");

    // Merge second TU.
    auto fid2 = unit->interested_file();
    merged.merge("tu1", tu_index.graph.include_location_id(fid2), tu_index.main_file_index, {});

    // Verify lookup finds the new occurrence (cache was invalidated).
    auto second_offset = point("second");
    bool found_second = false;
    merged.lookup(second_offset, [&](const index::Occurrence& occ) {
        if(occ.range.contains(second_offset))
            found_second = true;
        return true;
    });
    ASSERT_TRUE(found_second);
}

TEST_CASE(LocalSymbolTable) {
    build_index(R"(
            void foo() { int local = 42; }
            int global = 0;
        )");

    index::MergedIndex merged;
    auto main_path_id = static_cast<std::uint32_t>(tu_index.graph.paths.size() - 1);
    merged.merge("tu0", tu_index.built_at, {}, tu_index.main_file_index, "");

    // Collect non-External symbols from the TU that appear in the FileIndex.
    index::SymbolTable local_syms;
    for(auto& occ: tu_index.main_file_index.occurrences) {
        auto it = tu_index.symbols.find(occ.target);
        if(it != tu_index.symbols.end() && it->second.scope != index::SymbolScope::External) {
            local_syms.try_emplace(occ.target, it->second);
        }
    }
    ASSERT_FALSE(local_syms.empty());
    merged.merge_symbols(local_syms);

    // FileLocal symbols should be findable in the shard.
    std::string name;
    SymbolKind kind;
    bool found_local = false;
    for(auto& [hash, symbol]: local_syms) {
        if(symbol.name == "local") {
            ASSERT_TRUE(merged.find_symbol(hash, name, kind));
            ASSERT_EQ(name, "local");
            found_local = true;
        }
    }
    ASSERT_TRUE(found_local);

    // External symbol should NOT be in the shard's local table.
    for(auto& [hash, symbol]: tu_index.symbols) {
        if(symbol.name == "global") {
            ASSERT_FALSE(merged.find_symbol(hash, name, kind));
        }
    }
}

TEST_CASE(LocalSymbolSerialization) {
    build_index(R"(
            static int static_var = 0;
            void foo() { int local = 1; }
        )");

    index::MergedIndex merged;
    auto main_path_id = static_cast<std::uint32_t>(tu_index.graph.paths.size() - 1);
    merged.merge("tu0", tu_index.built_at, {}, tu_index.main_file_index, "");

    index::SymbolTable local_syms;
    for(auto& occ: tu_index.main_file_index.occurrences) {
        auto it = tu_index.symbols.find(occ.target);
        if(it != tu_index.symbols.end() && it->second.scope != index::SymbolScope::External) {
            local_syms.try_emplace(occ.target, it->second);
        }
    }
    ASSERT_FALSE(local_syms.empty());
    merged.merge_symbols(local_syms);

    // Serialize and deserialize.
    llvm::SmallString<4096> buf;
    {
        llvm::raw_svector_ostream os(buf);
        merged.serialize(os);
    }
    auto restored = index::MergedIndex(llvm::StringRef(buf.data(), buf.size()));

    // Symbols should survive round-trip (via buffer path).
    std::string name;
    SymbolKind kind;
    for(auto& [hash, symbol]: local_syms) {
        ASSERT_TRUE(restored.find_symbol(hash, name, kind));
        ASSERT_EQ(name, symbol.name);
    }
}

// The dep is backdated an hour so the merge (build_at = one minute ago)
// records its baseline hash; a later write bumps the mtime past build_at,
// so staleness reaches the Layer 2 content-hash check — exactly the branch
// these tests exercise. A dep newer than build_at gets no baseline at all
// (its content may postdate the indexed snapshot).
index::MergedIndex build_ctx_shard(llvm::StringRef dep_path) {
    namespace stdfs = std::filesystem;
    stdfs::last_write_time(dep_path.str(),
                           stdfs::file_time_type::clock::now() - std::chrono::hours(1));
    auto build_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch() - std::chrono::minutes(1));

    index::MergedIndex merged;
    index::FileIndex file_idx;
    index::DepLocation deps[] = {
        {.path = dep_path, .line = 1}
    };
    merged.merge("tu0", build_at, deps, file_idx, "");
    return merged;
}

TEST_CASE(TouchNoUpdate) {
    TempDir dir;
    auto dep = dir.path("dep.h");
    dir.touch("dep.h", "int shared = 1;");

    auto merged = build_ctx_shard(dep);

    // Same content, newer mtime — a pure touch must not trigger a reindex.
    ASSERT_FALSE(merged.need_update());

    // The buffer path (serialized shard) must reach the same conclusion.
    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    merged.serialize(os);
    auto restored = index::MergedIndex(llvm::StringRef(buf.data(), buf.size()));
    ASSERT_FALSE(restored.need_update());
}

TEST_CASE(ContentChangeUpdate) {
    TempDir dir;
    auto dep = dir.path("dep.h");
    dir.touch("dep.h", "int shared = 1;");

    auto merged = build_ctx_shard(dep);

    // Real edit: content hash diverges from the stored baseline.
    dir.touch("dep.h", "int shared = 2;");
    ASSERT_TRUE(merged.need_update());
}

TEST_CASE(OldShardDiscarded) {
    TempDir dir;

    // A current shard round-trips through disk and loads normally.
    {
        index::MergedIndex merged;
        index::FileIndex file_idx;
        merged.merge("tu0", std::chrono::milliseconds(1), {}, file_idx, "valid-shard");
        auto path = dir.path("valid.idx");
        std::error_code ec;
        llvm::raw_fd_ostream os(path, ec);
        merged.serialize(os);
        os.flush();
        ASSERT_TRUE(index::MergedIndex::load(path).content() == "valid-shard");
    }

    // A version-less (format_version=0) shard from an older build is silently
    // discarded — load returns an empty index, as if nothing were on disk.
    // Only the version slot is written: every other field reads back absent,
    // which is structurally valid — rejection must come from the version
    // check.
    {
        struct VersionOnly {
            std::uint32_t format_version = 0;
        };

        auto blob = kota::codec::fbs::to_bytes(VersionOnly{});
        ASSERT_TRUE(blob.has_value());

        auto path = dir.path("stale.idx");
        std::error_code ec;
        llvm::raw_fd_ostream os(path, ec);
        os.write(reinterpret_cast<const char*>(blob->data()), blob->size());
        os.flush();

        auto loaded = index::MergedIndex::load(path);
        ASSERT_TRUE(loaded.content().empty());
        ASSERT_TRUE(loaded.need_update());
    }

    // Positive control: the same single-slot shape carrying the CURRENT
    // version is kept — slot 0 really is the version slot and the rejection
    // above comes from its value, not from the blob's shape.
    {
        struct VersionOnly {
            std::uint32_t format_version = 0;
        };

        auto blob = kota::codec::fbs::to_bytes(VersionOnly{index::index_format_version});
        ASSERT_TRUE(blob.has_value());

        auto path = dir.path("current.idx");
        std::error_code ec;
        llvm::raw_fd_ostream os(path, ec);
        os.write(reinterpret_cast<const char*>(blob->data()), blob->size());
        os.flush();

        ASSERT_TRUE(index::MergedIndex::load(path).loaded());
    }
}

TEST_CASE(GarbageLoadRejected) {
    TempDir dir;
    dir.touch("garbage.idx", "not a flatbuffer");

    auto loaded = index::MergedIndex::load(dir.path("garbage.idx"));
    ASSERT_FALSE(loaded.loaded());
    ASSERT_TRUE(loaded.content().empty());
    ASSERT_TRUE(loaded.need_update());

    // Queries on the rejected shard answer with silence, not UB.
    bool visited = false;
    loaded.lookup(0, [&](const index::Occurrence&) {
        visited = true;
        return true;
    });
    ASSERT_FALSE(visited);

    loaded.lookup(index::SymbolHash(1), RelationKind::Definition, [&](const index::Relation&) {
        visited = true;
        return true;
    });
    ASSERT_FALSE(visited);

    std::string name;
    SymbolKind kind;
    ASSERT_FALSE(loaded.find_symbol(1, name, kind));
}

TEST_CASE(CorruptShardRejected) {
    TempDir dir;

    index::MergedIndex merged;
    index::FileIndex file_idx;
    merged.merge("tu0", std::chrono::milliseconds(1), {}, file_idx, "corrupt-me");

    llvm::SmallString<1024> blob;
    llvm::raw_svector_ostream os(blob);
    merged.serialize(os);
    ASSERT_TRUE(blob.size() > 8);

    auto write = [&](llvm::StringRef name, llvm::StringRef bytes) {
        dir.touch(name, bytes);
        return dir.path(name);
    };

    // Sanity: the intact bytes load, so the rejections below are earned.
    ASSERT_TRUE(index::MergedIndex::load(write("valid.idx", blob)).loaded());

    llvm::StringRef bytes(blob.data(), blob.size());
    ASSERT_FALSE(
        index::MergedIndex::load(write("half.idx", bytes.take_front(bytes.size() / 2))).loaded());
    ASSERT_FALSE(index::MergedIndex::load(write("minus1.idx", bytes.drop_back(1))).loaded());

    // Bytes 4-7 carry the buffer identifier; a blob from another format
    // must be rejected up front.
    std::string clobbered = bytes.str();
    for(std::size_t i = 4; i < 8; ++i) {
        clobbered[i] = 'X';
    }
    ASSERT_FALSE(index::MergedIndex::load(write("clobbered.idx", clobbered)).loaded());
}

TEST_CASE(OutOfRangeCanonicalIdRejected) {
    TempDir dir;

    // Field order MUST mirror the persisted shapes in merged_index.cpp
    // (MergedIndex::Impl prefix — skip-annotated fields occupy no slot —
    // HeaderContext, IncludeContext, CompilationContext prefix); the
    // trailing fields read back absent, which is structurally valid.
    struct IncludeContextMirror {
        std::uint32_t include_id = 0;
        std::uint32_t canonical_id = 0;
    };

    struct HeaderContextMirror {
        std::uint32_t version = 0;
        llvm::SmallVector<IncludeContextMirror> includes;
    };

    // The vector matters beyond field parity: without one the mirror would
    // be trivially copyable and encode as an inline struct, while the real
    // CompilationContext encodes as a table — the verifier tells them apart.
    struct CompilationContextMirror {
        std::uint32_t version = 0;
        std::uint32_t canonical_id = 0;
        std::uint64_t build_at = 0;
        std::vector<index::IncludeLocation> include_locations;
    };

    struct ReprMirror {
        std::uint32_t format_version = 0;
        std::vector<std::string> paths;
        std::string content;
        std::vector<std::uint32_t> line_starts;
        llvm::SmallDenseMap<std::uint32_t, HeaderContextMirror, 2> header_contexts;
        llvm::SmallDenseMap<std::uint32_t, CompilationContextMirror, 1> compilation_contexts;
        std::vector<std::pair<std::string, std::uint32_t>> canonical_cache;
        std::uint32_t max_canonical_id = 0;
    };

    // A consistent base: one path, one canonical id, one header context
    // referencing it.
    auto base = [] {
        ReprMirror mirror;
        mirror.format_version = index::index_format_version;
        mirror.max_canonical_id = 1;
        mirror.paths = {"/proj/tu.cpp"};
        mirror.canonical_cache.emplace_back("hash", 0);
        mirror.header_contexts[0].includes.push_back({.include_id = 0, .canonical_id = 0});
        return mirror;
    };

    // Structure and version pass, so load() accepts the blob off disk; the
    // first mutation materializes it in memory, where the id values face
    // the range check. Nullopt = the blob never reached that check.
    auto materialized_contribution = [&](llvm::StringRef name,
                                         const ReprMirror& mirror) -> std::optional<bool> {
        auto blob = kota::codec::fbs::to_bytes(mirror);
        if(!blob) {
            return std::nullopt;
        }
        dir.touch(name, llvm::StringRef(reinterpret_cast<const char*>(blob->data()), blob->size()));
        auto shard = index::MergedIndex::load(dir.path(name));
        if(!shard.loaded()) {
            return std::nullopt;
        }
        shard.remove("/proj/never-indexed.cpp");
        return shard.has_contribution("/proj/tu.cpp");
    };

    // Positive control first: the base materializes intact, so the
    // rejections below come from the hostile ids, not the blob's shape.
    auto good = materialized_contribution("good.idx", base());
    ASSERT_TRUE(good.has_value() && *good);

    // Structural verification does not constrain field values: each blob
    // carries one canonical id at or past max_canonical_id, which would
    // index canonical_ref_counts out of bounds if the in-memory load
    // accepted it. The blob is dropped and the shard reads as empty.
    auto bad_cache = base();
    bad_cache.canonical_cache.front().second = 5;
    auto cache_verdict = materialized_contribution("bad-cache.idx", bad_cache);
    ASSERT_TRUE(cache_verdict.has_value() && !*cache_verdict);

    auto bad_include = base();
    bad_include.header_contexts[0].includes.front().canonical_id = 5;
    auto include_verdict = materialized_contribution("bad-include.idx", bad_include);
    ASSERT_TRUE(include_verdict.has_value() && !*include_verdict);

    auto bad_compilation = base();
    bad_compilation.compilation_contexts[0].canonical_id = 5;
    auto compilation_verdict = materialized_contribution("bad-compilation.idx", bad_compilation);
    ASSERT_TRUE(compilation_verdict.has_value() && !*compilation_verdict);
}

TEST_CASE(BufferPathLookupParity) {
    build_index(R"(
            void §(a)alpha_func() {}
            int §(b)beta_var = 1;
        )");

    index::MergedIndex merged;
    auto fid = unit->interested_file();
    merged.merge("tu0",
                 tu_index.graph.include_location_id(fid),
                 tu_index.main_file_index,
                 unit->interested_content());

    auto hash_at = [&](llvm::StringRef pos) {
        auto offset = point(pos);
        index::SymbolHash hash = 0;
        merged.lookup(offset, [&](const index::Occurrence& occ) {
            hash = occ.target;
            return false;
        });
        return hash;
    };

    using Row = std::tuple<std::uint32_t, std::uint32_t, std::uint64_t>;
    auto definitions = [](index::MergedIndex& index, index::SymbolHash hash) {
        std::vector<Row> rows;
        index.lookup(hash, RelationKind::Definition, [&](const index::Relation& relation) {
            rows.emplace_back(relation.range.begin, relation.range.end, relation.target_symbol);
            return true;
        });
        std::ranges::sort(rows);
        return rows;
    };

    index::SymbolHash hashes[2] = {hash_at("a"), hash_at("b")};
    std::vector<Row> expected[2];
    for(std::size_t i = 0; i < 2; ++i) {
        ASSERT_TRUE(hashes[i] != 0);
        expected[i] = definitions(merged, hashes[i]);
        ASSERT_FALSE(expected[i].empty());
    }
    ASSERT_TRUE(hashes[0] != hashes[1]);

    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    merged.serialize(os);
    auto restored = index::MergedIndex(buf);

    // The zero-copy view is the production read path: it must agree BEFORE
    // anything materializes the impl (operator== would, so it comes last).
    for(std::size_t i = 0; i < 2; ++i) {
        ASSERT_TRUE(definitions(restored, hashes[i]) == expected[i]);
    }

    ASSERT_FALSE(restored.line_starts().empty());
    ASSERT_TRUE(std::ranges::equal(restored.line_starts(), merged.line_starts()));
}

TEST_CASE(BufferPathMultiOccurrenceLookup) {
    // Synthesized occurrences sorted by (begin, end, target), spaced so each
    // probe hits exactly one range (contains() is inclusive at both ends).
    index::FileIndex file_idx;
    index::SymbolHash target = 100;
    for(std::uint32_t begin = 0; begin < 60; begin += 10) {
        file_idx.occurrences.emplace_back(index::Range{begin, begin + 3}, target++);
    }

    index::MergedIndex merged;
    merged.merge("tu0", std::uint32_t(0), file_idx, "synthetic");

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    merged.serialize(os);
    auto restored = index::MergedIndex(buf);

    // The buffer path binary-searches the serialized rows: every probe must
    // land on exactly its own range.
    for(auto& occurrence: file_idx.occurrences) {
        std::vector<index::Occurrence> hits;
        restored.lookup(occurrence.range.begin + 1, [&](const index::Occurrence& hit) {
            hits.push_back(hit);
            return true;
        });
        ASSERT_EQ(hits.size(), 1u);
        ASSERT_TRUE(hits.front() == occurrence);
    }
}

std::uint64_t file_hash(llvm::StringRef path) {
    auto buf = llvm::MemoryBuffer::getFile(path);
    return buf ? llvm::xxh3_64bits((*buf)->getBuffer()) : 0;
}

/// A build_at far enough in the future that every existing file clears the
/// mtime guard and earns a stat fast path at merge.
std::chrono::milliseconds generous_build_at() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()) +
           std::chrono::milliseconds(10'000);
}

TEST_CASE(NeedUpdateChecksAllContexts) {
    TempDir tmp;
    tmp.touch("a.h", "int a();\n");
    tmp.touch("b.h", "int b();\n");
    auto a = tmp.path("a.h");
    auto b = tmp.path("b.h");

    index::MergedIndex shard;
    index::FileIndex fi_a, fi_b;
    auto dep_of = [&](const std::string& path) {
        return llvm::SmallVector<index::DepLocation>{
            {path, 1, 0, file_hash(path)}
        };
    };
    shard.merge("tuA", generous_build_at(), dep_of(a), fi_a, "int a();\n");
    shard.merge("tuB", generous_build_at(), dep_of(b), fi_b, "int b();\n");

    ASSERT_FALSE(shard.need_update());

    // Only one contribution's dependency goes stale at a time; a check that
    // stops at a single context would miss whichever the iteration order
    // hides, so exercise both.
    tmp.touch("b.h", "int b2();\n");
    ASSERT_TRUE(shard.need_update());

    shard.merge("tuB", generous_build_at(), dep_of(b), fi_b, "int b2();\n");
    ASSERT_FALSE(shard.need_update());
    tmp.touch("a.h", "int a2();\n");
    ASSERT_TRUE(shard.need_update());

    // The serialized reader shares the loop: both contexts again through a
    // reloaded view.
    shard.merge("tuA", generous_build_at(), dep_of(a), fi_a, "int a2();\n");
    llvm::SmallString<1024> s;
    llvm::raw_svector_ostream os(s);
    shard.serialize(os);
    auto view = index::MergedIndex(s);
    ASSERT_FALSE(view.need_update());

    // Same-size rewrites move the mtime explicitly: Windows file times
    // advance in ~16ms ticks, so a rewrite landing in the stamp's tick
    // reproduces size AND mtime exactly and the stat fast path rightly
    // trusts it. A real edit arrives long after the stamp; the bump
    // models that and pins these verdicts on the hash layer.
    tmp.touch("b.h", "int b3();\n");
    set_file_mtime(b, file_mtime_ns(b) + 5'000'000'000);
    ASSERT_TRUE(view.need_update());

    // Restore b (fresh again via the hash layer), then break a: the verdict
    // now hinges on the second context alone.
    tmp.touch("b.h", "int b2();\n");
    ASSERT_FALSE(view.need_update());
    tmp.touch("a.h", "int a3();\n");
    set_file_mtime(a, file_mtime_ns(a) + 5'000'000'000);
    ASSERT_TRUE(view.need_update());
}

TEST_CASE(NeedUpdateBackdatedEdit) {
    TempDir tmp;
    tmp.touch("dep.h", "int old_name();\n");
    auto dep = tmp.path("dep.h");

    index::MergedIndex shard;
    index::FileIndex fi;
    llvm::SmallVector<index::DepLocation> deps{
        {dep, 1, 0, file_hash(dep)}
    };
    shard.merge("tu", generous_build_at(), deps, fi, "content");
    ASSERT_FALSE(shard.need_update());

    // Same length, mtime rolled back: a watermark would call this fresh;
    // stamp equality sends it to the hash layer.
    auto recorded = file_mtime_ns(dep);
    tmp.touch("dep.h", "int new_name();\n");
    set_file_mtime(dep, recorded - 5'000'000'000);
    ASSERT_TRUE(shard.need_update());
}

TEST_CASE(SerializedStampsValidate) {
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");

    index::MergedIndex shard;
    index::FileIndex fi;
    llvm::SmallVector<index::DepLocation> deps{
        {dep, 1, 0, file_hash(dep)}
    };
    shard.merge("tu", generous_build_at(), deps, fi, "content");

    llvm::SmallString<1024> s;
    llvm::raw_svector_ostream os(s);
    shard.serialize(os);
    auto view = index::MergedIndex(s);

    ASSERT_FALSE(view.need_update());

    // Touched, not modified: the immutable stamp mismatches, the hash
    // proves the content unchanged.
    set_file_mtime(dep, file_mtime_ns(dep) + 5'000'000'000);
    ASSERT_FALSE(view.need_update());

    // A real edit is caught by the hash layer. The explicit mtime bump
    // keeps the same-size rewrite out of the stamp's Windows time tick
    // (see NeedUpdateChecksAllContexts).
    tmp.touch("dep.h", "int g();\n");
    set_file_mtime(dep, file_mtime_ns(dep) + 5'000'000'000);
    ASSERT_TRUE(view.need_update());
}

};  // TEST_SUITE(MergedIndex)
}  // namespace
}  // namespace clice::testing
