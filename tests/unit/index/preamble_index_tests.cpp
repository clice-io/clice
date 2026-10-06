module;

#include "modules/prelude.h"

module clice;

import :index.serialization;
import :index.shard;
import :index.tu_index;
import :project.project;
import :tests.unit.test.temp_dir;
import :tests.unit.test.test;
import :tests.unit.test.tester;

namespace clice::testing {

namespace {

/// The envelope's leading slots (the layout in tu_index.cpp): the version,
/// then the minimal path table verification demands — every other field
/// reads back absent, which is structurally valid.
struct VersionAndPaths {
    std::uint32_t format_version = 0;
    std::int64_t built_at = 0;
    std::vector<std::string> paths = {"/proj/main.cpp"};
};

ZEST_SUITE(PreambleIndex, Tester) {

TempDir dir;
std::shared_ptr<index::TUIndex> state;

std::vector<index::DocumentLink> links;
std::vector<std::uint32_t> inactive;
std::vector<std::uint8_t> conditionals;
std::string diagnostics;

/// Compile, build a preamble envelope, persist it as the `.pch.idx` pair
/// and load it back through the production gate.
void build_state(std::source_location location = std::source_location::current()) {
    ZASSERT(compile());

    links.resize(1);
    links[0].range = {12, 20};
    links[0].target = "/include/foo.h";
    inactive = {4, 9, 30, 42};
    conditionals = {1, 0, 2};
    diagnostics = R"([{"range":{},"message":"'M' macro redefined"}])";

    dir.touch("state.pch.idx",
              index::build_preamble_index(*unit, links, inactive, conditionals, diagnostics));
    state = load_pch_envelope(dir.path("state.pch.idx"));
    ZASSERT(state != nullptr);
}

index::SymbolHash hash_of(llvm::StringRef name,
                          std::source_location location = std::source_location::current()) {
    index::SymbolHash hash = 0;
    std::uint32_t count = 0;
    state->iterate_symbols(
        [&](index::SymbolHash symbol_id, const index::SymbolIdentity& symbol, llvm::StringRef) {
            if(symbol.name == name) {
                hash = symbol_id;
                count += 1;
            }
            return true;
        });
    ZEXPECT(count == 1);
    return hash;
}

/// Walk a symbol's relation rows in every header section (all but the
/// main file's), the way the query layer's overlay lookup serves them.
void lookup_headers(index::SymbolHash hash,
                    RelationKind kind,
                    llvm::function_ref<bool(llvm::StringRef, const index::Relation&)> callback) {
    for(std::uint32_t i = 0; i < state->section_count(); i += 1) {
        auto path_id = state->section_path(i);
        if(path_id == state->path_count() - 1) {
            continue;
        }
        bool keep = true;
        state->shard_of(path_id).lookup(hash, kind, [&](const index::Relation& r) {
            keep = callback(state->path(path_id), r);
            return keep;
        });
        if(!keep) {
            return;
        }
    }
}

ZEST_CASE(ForcedIncludeServed) {
    add_file("forced.h", R"(int §(def)⟦forced_value⟧ = 1;)");
    add_main("main.cpp", R"(int x = forced_value;)");

    // A compile-command forced include: clang records its include edge in
    // the predefines buffer, which is a valid location — so unlike the
    // synthetic buffers themselves, the file must stay in the envelope
    // under its own path.
    prepare();
    owned_args.insert(owned_args.end() - 1, "-include");
    owned_args.insert(owned_args.end() - 1, TestVFS::path("forced.h"));
    params.arguments.clear();
    for(auto& arg: owned_args) {
        params.arguments.push_back(arg.c_str());
    }
    ZASSERT(try_compile());

    dir.touch("state.pch.idx", index::build_preamble_index(*unit, {}, {}, {}, {}));
    state = load_pch_envelope(dir.path("state.pch.idx"));
    ZASSERT(state != nullptr);

    bool found = false;
    lookup_headers(hash_of("forced_value"),
                   RelationKind::Definition,
                   [&](llvm::StringRef path, const index::Relation& r) {
                       ZEXPECT(path.ends_with("forced.h"));
                       ZEXPECT(dump(r.range) == dump(range("def", "forced.h")));
                       found = true;
                       return false;
                   });
    ZEXPECT(found);
}

ZEST_CASE(HeaderRelationLookup) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
inline void bar() { §(href)⟦foo⟧(); }
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦foo⟧(); return 0; }
)");
    build_state();

    auto foo = hash_of("foo");

    // The definition inside the header is served from its section.
    bool found_def = false;
    lookup_headers(foo,
                   RelationKind::Definition,
                   [&](llvm::StringRef path, const index::Relation& r) {
                       ZEXPECT(path.ends_with("foo.h"));
                       ZEXPECT(dump(r.range) == dump(range("def", "foo.h")));
                       found_def = true;
                       return false;
                   });
    ZEXPECT(found_def);

    // Header-internal references are in the envelope too.
    bool found_ref = false;
    lookup_headers(foo, RelationKind::Reference, [&](llvm::StringRef, const index::Relation& r) {
        if(r.range == range("href", "foo.h")) {
            found_ref = true;
            return false;
        }
        return true;
    });
    ZEXPECT(found_ref);

    // Everything needed to map rows to LSP positions rides in each shard;
    // pure-ASCII content itself is omitted.
    for(std::uint32_t i = 0; i < state->section_count(); i += 1) {
        auto& shard = state->shard_of(state->section_path(i));
        ZEXPECT(shard.content_size() > 0);
        ZEXPECT(!shard.line_starts().empty());
        ZEXPECT(shard.content().empty());
        ZEXPECT(shard.content().empty());
    }
}

ZEST_CASE(PreambleLookup) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦§(ref)foo⟧(); return 0; }
)");
    build_state();

    auto foo = hash_of("foo");
    const index::Shard& preamble = state->shard_of(state->path_count() - 1);
    ZASSERT(preamble.loaded());

    // Occurrence lookup by offset in the preamble entry.
    bool found_occurrence = false;
    preamble.lookup(point("ref"), [&](const index::Occurrence& occurrence) {
        ZEXPECT(occurrence.target == foo);
        ZEXPECT(dump(occurrence.range) == dump(range("ref")));
        found_occurrence = true;
        return false;
    });
    ZEXPECT(found_occurrence);

    // Relation lookup by symbol in the preamble entry.
    bool found_relation = false;
    preamble.lookup(foo, RelationKind::Reference, [&](const index::Relation& r) {
        ZEXPECT(dump(r.range) == dump(range("ref")));
        found_relation = true;
        return false;
    });
    ZEXPECT(found_relation);
}

ZEST_CASE(SymbolTableLookup) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦foo⟧(); return 0; }
)");
    build_state();

    auto foo = hash_of("foo");

    auto identity = state->find_symbol(foo);
    ZASSERT(identity);
    ZEXPECT(identity->name == "foo");
    ZEXPECT(identity->kind.value() == SymbolKind(SymbolKind::Function).value());

    ZEXPECT(!state->find_symbol(foo + 1).has_value());
}

ZEST_CASE(FeatureStateRoundtrip) {
    add_main("main.cpp", R"(
int main() { return 0; }
)");
    build_state();

    auto loaded_links = state->links();
    ZASSERT(loaded_links.size() == 1);
    ZEXPECT(loaded_links[0].range == LocalSourceRange(12, 20));
    ZEXPECT(loaded_links[0].target == "/include/foo.h");

    ZEXPECT(state->inactive_regions() == llvm::ArrayRef<std::uint32_t>(inactive));
    ZEXPECT(state->open_conditionals() == llvm::ArrayRef<std::uint8_t>(conditionals));
    ZEXPECT(state->preamble_diagnostics() == diagnostics);

    // An envelope with no header sections answers lookups with silence,
    // not UB.
    bool visited = false;
    lookup_headers(42, RelationKind::Reference, [&](llvm::StringRef, const index::Relation&) {
        visited = true;
        return true;
    });
    ZEXPECT(!visited);
}

ZEST_CASE(RejectBadBlob) {
    ZEXPECT(load_pch_envelope(dir.path("missing.pch.idx")) == nullptr);

    dir.touch("garbage.pch.idx", "not a flatbuffer at all");
    ZEXPECT(load_pch_envelope(dir.path("garbage.pch.idx")) == nullptr);
}

ZEST_CASE(RejectVersionMismatch) {
    // A structurally valid blob of another format version — none (0, what a
    // version-less blob reads back) or the one before this build's — must
    // load as missing, so the PCH pair rebuilds instead of serving a stale
    // layout.
    for(auto version: {0u, index::index_format_version - 1}) {
        auto blob = kota::codec::fbs::to_bytes(VersionAndPaths{.format_version = version});
        ZASSERT(blob);

        dir.touch("stale.pch.idx",
                  llvm::StringRef(reinterpret_cast<const char*>(blob->data()), blob->size()));
        ZEXPECT(load_pch_envelope(dir.path("stale.pch.idx")) == nullptr);
    }
}

ZEST_CASE(AcceptCurrentVersionBlob) {
    // Positive control for RejectVersionMismatch: the same blob carrying the
    // CURRENT version loads — the rejection comes from the version's value,
    // not from the blob's shape.
    auto blob =
        kota::codec::fbs::to_bytes(VersionAndPaths{.format_version = index::index_format_version});
    ZASSERT(blob);

    dir.touch("current.pch.idx",
              llvm::StringRef(reinterpret_cast<const char*>(blob->data()), blob->size()));
    ZEXPECT(load_pch_envelope(dir.path("current.pch.idx")) != nullptr);
}

ZEST_CASE(RejectCorruptBlob) {
    add_main("main.cpp", R"(
int main() { return 0; }
)");
    build_state();

    auto read = read_file(dir.path("state.pch.idx"));
    ZASSERT(read);
    llvm::StringRef bytes = *read;
    ZASSERT(bytes.size() > 8);

    dir.touch("truncated.pch.idx", bytes.take_front(bytes.size() / 2));
    ZEXPECT(load_pch_envelope(dir.path("truncated.pch.idx")) == nullptr);

    // Bytes 4-7 carry the buffer identifier; a blob from another format
    // must be rejected up front.
    std::string clobbered = bytes.str();
    for(std::size_t i = 4; i < 8; i += 1) {
        clobbered[i] = 'X';
    }
    dir.touch("clobbered.pch.idx", clobbered);
    ZEXPECT(load_pch_envelope(dir.path("clobbered.pch.idx")) == nullptr);
}

ZEST_CASE(RejectCorruptSectionBlob) {
    add_main("main.cpp", R"(
int main() { return 0; }
)");
    build_state();

    // Overwrite one section's blob bytes in place: the envelope stays
    // structurally valid, but the load gate verifies every blob and must
    // read the pair as missing instead of silently serving nothing.
    auto read = read_file(dir.path("state.pch.idx"));
    ZASSERT(read);
    std::string bytes = std::move(*read);

    auto view = index::TUIndex::from_bytes(bytes);
    ZASSERT(view.loaded());
    ZASSERT(view.section_count() > 0);
    auto blob = view.section_blob(0);
    auto pos = llvm::StringRef(bytes).find(blob);
    ZASSERT(pos != llvm::StringRef::npos);
    for(std::size_t i = 0; i < blob.size(); i += 1) {
        bytes[pos + i] = 'X';
    }

    dir.touch("bad_section.pch.idx", bytes);
    ZEXPECT(load_pch_envelope(dir.path("bad_section.pch.idx")) == nullptr);
}

ZEST_CASE(SourcePathAndPrefix) {
    add_main("main.cpp", R"(
int value = 42;
int other = 1;
)");
    build_state();

    ZEXPECT(state->path(state->path_count() - 1).ends_with("main.cpp"));

    // The preamble text itself is not stored; the envelope keeps only the
    // identity of the exact prefix it was built from.
    auto content = unit->main_content();
    ZEXPECT(state->matches_prefix(content));
    ZEXPECT(state->matches_prefix(content.str() + "\nint more = 2;"));
    ZEXPECT(!state->matches_prefix(content.drop_back(1)));
    ZEXPECT(!state->matches_prefix("int changed = 0;"));

    // An ordinary envelope never serves preamble state.
    auto ordinary = index::build_tu_index(*unit);
    ZEXPECT(!index::TUIndex::from_bytes(ordinary).matches_prefix(content));
}

};  // ZEST_SUITE(PreambleIndex)

}  // namespace

}  // namespace clice::testing
