module clice;

import :feature.inactive_regions;
import :index.instantiation_shard;
import :index.project_index;
import :index.shard;
import :server.served_project;
import :support.bytes;

namespace clice {

namespace {

/// The preamble index of PCH `key`, loaded on its first read: the live
/// sources are views, and the load only fills the project's cache.
std::shared_ptr<index::TUIndex> preamble_of(const Project& project, llvm::StringRef key) {
    return const_cast<Project&>(project).preamble_index(key);
}

/// An open document's index as the live sources hand it out: `shard` in
/// the buffer's coordinates, with the instantiations that serve beside it.
index::IndexSource buffer_source(const ServedProject& served,
                                 index::IndexSource::Kind kind,
                                 const ServedDocument& document,
                                 const index::Shard& shard) {
    auto& project = served.project;
    auto& session = *document.session;
    auto file = session.path_id;
    index::IndexSource source{
        .kind = kind,
        .file = file,
        .path = project.file_table.display(file),
        .shard = &shard,
        .positions = session.positions(),
    };
    // The buffer's own compile resolved what it instantiates and expands;
    // what the other units do in the file serves while the buffer is the
    // text they read.
    if(kind == index::IndexSource::Kind::SessionShard) {
        auto& state = *document.latest->index;
        if(auto section = state.instantiation_section_of(state.focus_file())) {
            source.instantiations.push_back(&state.instantiation_shard(*section));
        }
    }
    auto* stored = project.index.instantiation_shard(file);
    if(kind != index::IndexSource::Kind::PreambleShard && stored &&
       stored->content_hash() == session.hash) {
        source.instantiations.push_back(stored);
    }
    return source;
}

/// A shard's entries counted and hashed, whichever variants hold them: a
/// merge only adds to the entries of the shards it takes, so equal counts
/// are equal entries.
struct Entries {
    std::size_t count = 0;
    std::uint64_t hash = 0;
};

Entries entries_of(const index::Shard& shard) {
    Entries entries;
    shard.for_each_occurrence([&](const index::Occurrence& occurrence) {
        entries.count += 1;
        entries.hash = llvm::hash_combine(entries.hash,
                                          occurrence.range.begin,
                                          occurrence.range.end,
                                          occurrence.target);
        return true;
    });
    shard.for_each_relation([&](index::SymbolHash symbol, const index::Relation& relation) {
        entries.count += 1;
        entries.hash = llvm::hash_combine(entries.hash,
                                          symbol,
                                          static_cast<std::uint32_t>(relation.kind),
                                          relation.range.begin,
                                          relation.range.end,
                                          relation.target);
        return true;
    });
    return entries;
}

/// The open document's own file index when it describes the buffer: the
/// compile is current, or it compiled these very bytes. While the buffer
/// is also the text the file's shard indexed, it is merged with the
/// shard's entries in the code the compile skipped: the compile indexed
/// the one context it ran in, the shard every context the file compiles
/// in.
const index::Shard* buffer_shard(const ServedProject& served, const ServedDocument& document) {
    auto& compiled = document.latest;
    if(!compiled || !compiled->index || !compiled->index->loaded()) {
        return nullptr;
    }
    // A shard compiled from exactly the buffer's bytes describes it however
    // its dependencies moved since (the shard's DepsOnly contract).
    auto& state = *compiled->index;
    auto& shard = state.shard_of(state.focus_file());
    auto& session = *document.session;
    if(!served.current(session.path_id) &&
       !shard.matches_content(session.text().size(), session.hash)) {
        return nullptr;
    }
    auto* stored = served.matching_shard(session);
    auto& skipped = compiled->inactive_regions;
    if(skipped.empty() || !stored || stored->content_hash() != shard.content_hash()) {
        document.merged.reset();
        return &shard;
    }
    if(!document.merged || document.merged->compile != compiled) {
        auto keep = served.project.index.live_variants(session.path_id);
        keep.erase(ranges::remove(keep, shard.variants().front()).begin(), keep.end());
        auto bytes = index::merge_shards(
            *stored,
            keep,
            llvm::ArrayRef(&shard, 1),
            [&](std::uint32_t offset) { return feature::inactive_at(skipped, offset); });
        auto merged =
            index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(as_chars(bytes)));
        auto entries = entries_of(merged);
        document.merged = ServedDocument::MergedIndex{
            .compile = compiled,
            .shard = std::move(merged),
        };
        if(entries.count != entries_of(shard).count) {
            document.merged->additions = entries.hash;
        }
    }
    return &document.merged->shard;
}

/// The overlay TU index of an open document's PCH, or null when it has no
/// PCH or the index is unreadable. Returned by value: a reference into the
/// map value would not survive a rehash.
std::shared_ptr<index::TUIndex> overlay_of(const ServedProject& served,
                                           const ServedDocument& document) {
    if(!document.latest || !document.latest->loaded_pch()) {
        return nullptr;
    }
    return preamble_of(served.project, *document.latest->loaded_pch());
}

}  // namespace

bool ServedProject::is_open(Fid file) const {
    return find_document(file) != nullptr;
}

const index::Shard* ServedProject::matching_shard(const Session& session) const {
    auto* shard = project.index.shard(session.path_id);
    return shard && shard->matches_content(session.text().size(), session.hash) ? shard : nullptr;
}

std::optional<index::IndexSource> ServedProject::buffer_index(Fid file) const {
    auto* document = find_document(file);
    if(!document) {
        return std::nullopt;
    }
    if(auto* shard = buffer_shard(*this, *document)) {
        return buffer_source(*this, index::IndexSource::Kind::SessionShard, *document, *shard);
    }
    auto* shard = matching_shard(*document->session);
    if(!shard) {
        return std::nullopt;
    }
    return buffer_source(*this, index::IndexSource::Kind::Shard, *document, *shard);
}

ServedProject::SkippedCode ServedProject::skipped_code(Fid file) const {
    auto* document = find_document(file);
    if(!document || !buffer_shard(*this, *document) || !document->merged) {
        return {};
    }
    return {
        .regions = document->latest->inactive_regions,
        .indexed = document->merged->additions != 0,
    };
}

bool ServedProject::merge_again(Fid file) const {
    auto* document = find_document(file);
    if(!document) {
        return false;
    }
    auto before = document->merged ? document->merged->additions : 0;
    document->merged.reset();
    buffer_shard(*this, *document);
    return before != (document->merged ? document->merged->additions : 0);
}

void ServedProject::for_each_session(
    llvm::function_ref<bool(const index::IndexSource&)> visit) const {
    for(auto& [file, document]: documents) {
        auto* shard = buffer_shard(*this, document);
        if(shard &&
           !visit(buffer_source(*this, index::IndexSource::Kind::SessionShard, document, *shard))) {
            return;
        }
    }
}

void ServedProject::for_each_session_index(
    llvm::function_ref<bool(const index::TUIndex&)> visit) const {
    for(auto& [file, document]: documents) {
        if(buffer_shard(*this, document) && !visit(*document.latest->index)) {
            return;
        }
    }
}

void ServedProject::for_each_preamble(
    llvm::function_ref<bool(const index::IndexSource&)> visit) const {
    for(auto& [file, document]: documents) {
        auto state = overlay_of(*this, document);
        if(!state) {
            continue;
        }
        // The preamble shard's entries are buffer offsets of the file that
        // built the blob: serve them only for that very file and only while
        // the buffer still starts with the exact preamble text the blob was
        // built from. The prefix comparison validates the described region
        // directly — body edits never move preamble entries — so no dirty-flag
        // gating is needed on top.
        auto last = state->focus_file();
        if(project.file_table.find(Spelling::absolute(state->path(last))) != file ||
           !state->matches_preamble(document.session->text())) {
            continue;
        }
        if(!visit(buffer_source(*this,
                                index::IndexSource::Kind::PreambleShard,
                                document,
                                state->shard_of(last)))) {
            return;
        }
    }
}

void ServedProject::for_each_overlay(llvm::function_ref<bool(const index::TUIndex&)> visit) const {
    // Sessions with identical preambles share one blob; visit it once. The
    // build's precompiled header a preamble's PCH is built over has none:
    // what it holds is indexed with the unit writing it.
    llvm::StringSet<> seen;
    for(auto& [file, document]: documents) {
        auto& compiled = document.latest;
        if(!compiled || !compiled->loaded_pch() || !seen.insert(*compiled->loaded_pch()).second) {
            continue;
        }
        if(auto state = preamble_of(project, *compiled->loaded_pch()); state && !visit(*state)) {
            return;
        }
    }
}

std::shared_ptr<index::TUIndex> ServedProject::preamble_index(Fid file) const {
    auto* document = find_document(file);
    if(!document) {
        return nullptr;
    }
    auto state = overlay_of(*this, *document);
    if(!state || !state->matches_preamble(document->session->text())) {
        return nullptr;
    }
    return state;
}

}  // namespace clice
