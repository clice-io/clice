#include <filesystem>

#include "schema_generated.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "test/tester.h"
#include "index/merged_index.h"

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
            if(relation.kind & RelationKind(RelationKind::Definition)) {
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
            if(relation.kind & RelationKind(RelationKind::Definition)) {
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
    {
        namespace binary = clice::index::binary;
        flatbuffers::FlatBufferBuilder builder;
        auto content = builder.CreateString("stale-shard");
        auto paths = builder.CreateVector<flatbuffers::Offset<flatbuffers::String>>({});
        auto cache = builder.CreateVector<flatbuffers::Offset<binary::CacheEntry>>({});
        auto headers = builder.CreateVector<flatbuffers::Offset<binary::HeaderContextEntry>>({});
        auto compilations =
            builder.CreateVector<flatbuffers::Offset<binary::CompilationContextEntry>>({});
        auto occurrences = builder.CreateVector<flatbuffers::Offset<binary::OccurrenceEntry>>({});
        auto relations =
            builder.CreateVector<flatbuffers::Offset<binary::SymbolRelationsEntry>>({});
        auto root = binary::CreateMergedIndex(builder,
                                              0,
                                              paths,
                                              cache,
                                              headers,
                                              compilations,
                                              occurrences,
                                              relations,
                                              0,
                                              content,
                                              0,
                                              0,
                                              /*format_version=*/0);
        builder.Finish(root);

        auto path = dir.path("stale.idx");
        std::error_code ec;
        llvm::raw_fd_ostream os(path, ec);
        os.write(reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize());
        os.flush();

        auto loaded = index::MergedIndex::load(path);
        ASSERT_TRUE(loaded.content().empty());
        ASSERT_TRUE(loaded.need_update());
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
