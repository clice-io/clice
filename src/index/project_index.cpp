#include "index/project_index.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "index/serialization.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"

namespace clice::index {

namespace {

/// The global layer's persisted form: the FileVersion table as parallel
/// columns plus the symbol table with a self-contained path table for its
/// reference bitmaps.
struct GlobalBlob {
    std::uint32_t format_version = 0;

    /// See ProjectIndex::global_generation.
    std::uint64_t generation = 0;

    std::uint32_t next_fv_id = 0;

    std::vector<std::uint32_t> fv_ids;
    std::vector<std::string> fv_paths;
    std::vector<std::uint64_t> fv_hashes;
    std::vector<std::uint64_t> fv_sizes;
    std::vector<std::int64_t> fv_mtimes;

    /// The external symbol table as parallel columns. Reference bitmaps
    /// travel as raw portable images rather than through the Bitmap repr:
    /// the repr has no failure channel and would normalize a malformed
    /// image to empty — silently losing the symbol's reference files with
    /// nothing ever rebuilding them — while raw images let load_global
    /// reject the blob so everything is reindexed.
    std::vector<std::uint64_t> sym_hashes;
    std::vector<std::string> sym_names;
    std::vector<std::uint8_t> sym_kinds;
    std::vector<std::vector<std::byte>> sym_bitmaps;

    /// tu_fv -> generation stamp of every manifest current at this save.
    /// The loader adopts a manifest blob only on an exact stamp match, so
    /// a manifest whose write failed while the global landed (or one that
    /// outran a lost global write) reads as lost and its TU reindexes,
    /// instead of an older on-disk manifest serving as current.
    std::vector<std::uint32_t> manifest_fvs;
    std::vector<std::uint64_t> manifest_gens;

    /// Pool id -> path for every id the symbol bitmaps reference.
    std::vector<std::pair<std::uint32_t, std::string>> sym_paths;
};

}  // namespace

bool ProjectIndex::merge(this ProjectIndex& self,
                         const TUIndexView& view,
                         llvm::ArrayRef<std::uint32_t> file_ids_map) {
    // Decode and bound every reference bitmap before touching the table:
    // merged bits persist in the global blob while the result's recorded
    // versions all match the disk, so a malformed image normalized to
    // empty — or a silently dropped out-of-range id, whose relations would
    // sit in a shard the symbol's fan-out never visits — would lose
    // reference files with nothing ever rebuilding them. Either rejects
    // the whole result instead — and the reject must leave no partial
    // names or bits behind, hence the staging.
    struct StagedSymbol {
        SymbolHash hash;
        SymbolIdentity identity;
        Bitmap references;
    };

    std::vector<StagedSymbol> staged;
    bool valid = true;
    view.iterate_symbols(
        [&](SymbolHash hash, const SymbolIdentity& identity, llvm::StringRef bitmap) {
            if(!valid || identity.scope != SymbolScope::External) {
                return;
            }
            Bitmap references;
            if(!bitmap.empty()) {
                auto decoded = read_bitmap(bitmap.data(), bitmap.size());
                if(!decoded) {
                    valid = false;
                    return;
                }
                references = std::move(*decoded);
            }
            if(!references.isEmpty() && references.maximum() >= file_ids_map.size()) {
                valid = false;
                return;
            }
            staged.push_back({hash, identity, std::move(references)});
        });
    if(!valid) {
        return false;
    }

    for(auto& [hash, identity, references]: staged) {
        auto& target = self.symbols[hash];
        if(target.name.empty()) {
            target.name = std::string(identity.name);
            target.kind = identity.kind;
        }
        for(auto ref: references) {
            target.reference_files.add(file_ids_map[ref]);
        }
    }

    return true;
}

std::uint32_t ProjectIndex::intern_file_version(this ProjectIndex& self,
                                                std::uint32_t path_id,
                                                std::uint64_t content_hash) {
    auto [it, inserted] = self.fv_ids.try_emplace({path_id, content_hash}, self.next_fv_id);
    if(inserted) {
        self.file_versions.try_emplace(
            self.next_fv_id,
            FileVersionRecord{.path_id = path_id, .content_hash = content_hash});
        self.next_fv_id += 1;
    }
    return it->second;
}

bool ProjectIndex::knows_file_versions(this const ProjectIndex& self, const TUManifest& manifest) {
    if(!self.file_versions.contains(manifest.tu_fv)) {
        return false;
    }
    for(auto& node: manifest.nodes) {
        if(!self.file_versions.contains(node.fv)) {
            return false;
        }
    }
    for(auto& [fv, hash]: manifest.contributions) {
        if(!self.file_versions.contains(fv)) {
            return false;
        }
    }
    return true;
}

llvm::SmallVector<std::uint32_t> ProjectIndex::apply_manifest(this ProjectIndex& self,
                                                              std::uint32_t tu_path_id,
                                                              TUManifest manifest) {
    llvm::SmallVector<std::uint32_t> affected = self.remove_manifest(tu_path_id);

    for(auto& [fv, hash]: manifest.contributions) {
        auto path_id = self.file_versions.at(fv).path_id;
        self.contributions[path_id][tu_path_id] = hash;
        affected.push_back(path_id);
    }
    self.manifests[tu_path_id] = std::move(manifest);

    llvm::sort(affected);
    affected.erase(llvm::unique(affected), affected.end());
    return affected;
}

llvm::SmallVector<std::uint32_t> ProjectIndex::remove_manifest(this ProjectIndex& self,
                                                               std::uint32_t tu_path_id) {
    llvm::SmallVector<std::uint32_t> affected;
    auto it = self.manifests.find(tu_path_id);
    if(it == self.manifests.end()) {
        return affected;
    }

    for(auto& [fv, hash]: it->second.contributions) {
        auto path_id = self.file_versions.at(fv).path_id;
        auto contribution_it = self.contributions.find(path_id);
        if(contribution_it == self.contributions.end()) {
            continue;
        }
        contribution_it->second.erase(tu_path_id);
        if(contribution_it->second.empty()) {
            self.contributions.erase(contribution_it);
        }
        affected.push_back(path_id);
    }
    self.manifests.erase(it);

    llvm::sort(affected);
    affected.erase(llvm::unique(affected), affected.end());
    return affected;
}

llvm::SmallVector<std::uint64_t> ProjectIndex::live_variants(this const ProjectIndex& self,
                                                             std::uint32_t path_id) {
    llvm::SmallVector<std::uint64_t> variants;
    auto it = self.contributions.find(path_id);
    if(it == self.contributions.end()) {
        return variants;
    }
    for(auto hash: llvm::make_second_range(it->second)) {
        variants.push_back(hash);
    }
    llvm::sort(variants);
    variants.erase(llvm::unique(variants), variants.end());
    return variants;
}

void ProjectIndex::serialize_global(this ProjectIndex& self,
                                    llvm::raw_ostream& os,
                                    const clice::PathPool& pool) {
    // Garbage-collect the FileVersion table down to what some manifest
    // still references — in memory too, so ids of dead versions stop
    // accumulating across the session (they are never reused either way).
    llvm::DenseSet<std::uint32_t> referenced;
    for(auto& manifest: llvm::make_second_range(self.manifests)) {
        referenced.insert(manifest.tu_fv);
        for(auto& node: manifest.nodes) {
            referenced.insert(node.fv);
        }
        for(auto& [fv, hash]: manifest.contributions) {
            referenced.insert(fv);
        }
    }

    llvm::SmallVector<std::uint32_t> dead;
    for(auto id: llvm::make_first_range(self.file_versions)) {
        if(!referenced.contains(id)) {
            dead.push_back(id);
        }
    }
    for(auto id: dead) {
        auto& record = self.file_versions.at(id);
        self.fv_ids.erase({record.path_id, record.content_hash});
        self.file_versions.erase(id);
    }

    GlobalBlob blob;
    blob.format_version = index_format_version;
    blob.generation = self.global_generation;
    blob.next_fv_id = self.next_fv_id;

    llvm::SmallVector<std::uint32_t> ids;
    ids.reserve(self.file_versions.size());
    for(auto id: llvm::make_first_range(self.file_versions)) {
        ids.push_back(id);
    }
    llvm::sort(ids);
    for(auto id: ids) {
        auto& record = self.file_versions.at(id);
        blob.fv_ids.push_back(id);
        blob.fv_paths.emplace_back(pool.resolve(record.path_id));
        blob.fv_hashes.push_back(record.content_hash);
        blob.fv_sizes.push_back(record.size);
        blob.fv_mtimes.push_back(record.mtime_ns);
    }

    blob.manifest_fvs.reserve(self.manifests.size());
    blob.manifest_gens.reserve(self.manifests.size());
    for(auto& manifest: llvm::make_second_range(self.manifests)) {
        blob.manifest_fvs.push_back(manifest.tu_fv);
        blob.manifest_gens.push_back(manifest.global_gen);
    }

    Bitmap bitmap_referenced;
    blob.sym_hashes.reserve(self.symbols.size());
    blob.sym_names.reserve(self.symbols.size());
    blob.sym_kinds.reserve(self.symbols.size());
    blob.sym_bitmaps.reserve(self.symbols.size());
    for(auto& [hash, symbol]: self.symbols) {
        blob.sym_hashes.push_back(hash);
        // Moved out for the write and moved back below; the table itself
        // stays untouched in between so the two iterations pair up.
        blob.sym_names.push_back(std::move(symbol.name));
        blob.sym_kinds.push_back(symbol.kind.value());
        blob.sym_bitmaps.push_back(write_bitmap(symbol.reference_files));
        bitmap_referenced |= symbol.reference_files;
    }
    blob.sym_paths.reserve(bitmap_referenced.cardinality());
    for(auto id: bitmap_referenced) {
        blob.sym_paths.emplace_back(id, pool.resolve(id).str());
    }

    serialize_blob(blob, os);

    std::size_t i = 0;
    for(auto& symbol: llvm::make_second_range(self.symbols)) {
        symbol.name = std::move(blob.sym_names[i]);
        i += 1;
    }
}

bool ProjectIndex::load_global(this ProjectIndex& self,
                               llvm::StringRef data,
                               clice::PathPool& pool,
                               llvm::DenseMap<std::uint32_t, std::uint64_t>& manifest_pins) {
    GlobalBlob blob;
    if(!deserialize_blob(data, blob) || blob.format_version != index_format_version) {
        return false;
    }

    auto count = blob.fv_ids.size();
    if(blob.fv_paths.size() != count || blob.fv_hashes.size() != count ||
       blob.fv_sizes.size() != count || blob.fv_mtimes.size() != count) {
        return false;
    }
    auto sym_count = blob.sym_hashes.size();
    if(blob.sym_names.size() != sym_count || blob.sym_kinds.size() != sym_count ||
       blob.sym_bitmaps.size() != sym_count) {
        return false;
    }
    if(blob.manifest_gens.size() != blob.manifest_fvs.size()) {
        return false;
    }

    // Every value check runs before the first mutation: a blob rejected
    // halfway through would otherwise leave partial state behind — file
    // versions whose corrupt stat stamps feed the freshness fast path, and
    // symbols the next global save would persist — while the caller treats
    // the failed load as "no index on disk".

    // The writer only emits interned paths, which are never empty; an
    // empty entry marks a corrupt blob and must not become a real pool
    // entry.
    for(auto& path: blob.fv_paths) {
        if(path.empty()) {
            return false;
        }
    }
    for(auto& path: llvm::make_second_range(blob.sym_paths)) {
        if(path.empty()) {
            return false;
        }
    }

    // Ids and (path, hash) pairs are both map keys in the writer, so a
    // repeat of either marks a corrupt blob. A repeated id in particular
    // would leave fv_ids interning the earlier pair to an id whose record
    // names the later path, attributing contributions to the wrong file.
    llvm::DenseSet<std::uint32_t> blob_fvs(blob.fv_ids.begin(), blob.fv_ids.end());
    if(blob_fvs.size() != count) {
        return false;
    }
    llvm::DenseSet<std::pair<llvm::StringRef, std::uint64_t>> blob_versions;
    for(std::size_t i = 0; i < count; i += 1) {
        if(!blob_versions.insert({llvm::StringRef(blob.fv_paths[i]), blob.fv_hashes[i]}).second) {
            return false;
        }
    }

    // The writer only pins manifests whose tu_fv survived the same save's
    // garbage collection, so an unresolvable pin marks a corrupt blob.
    for(auto fv: blob.manifest_fvs) {
        if(!blob_fvs.contains(fv)) {
            return false;
        }
    }

    // The writer emits a path-table entry for every id its bitmaps
    // reference; an uncovered id dropped here would silently lose the
    // symbol's reference files with every manifest still fresh — reject
    // the blob so everything is reindexed instead.
    llvm::DenseSet<std::uint32_t> covered;
    for(auto id: llvm::make_first_range(blob.sym_paths)) {
        covered.insert(id);
    }
    std::vector<Bitmap> bitmaps;
    bitmaps.reserve(sym_count);
    for(auto& image: blob.sym_bitmaps) {
        auto decoded = read_bitmap(image.data(), image.size());
        if(!decoded) {
            return false;
        }
        for(auto id: *decoded) {
            if(!covered.contains(id)) {
                return false;
            }
        }
        bitmaps.push_back(std::move(*decoded));
    }

    self.global_generation = blob.generation;
    self.next_fv_id = blob.next_fv_id;
    for(std::size_t i = 0; i < count; i += 1) {
        auto path_id = pool.intern(blob.fv_paths[i]);
        auto id = blob.fv_ids[i];
        self.file_versions[id] = {path_id, blob.fv_hashes[i], blob.fv_sizes[i], blob.fv_mtimes[i]};
        self.fv_ids[{path_id, blob.fv_hashes[i]}] = id;
        // Ids must stay unique forever; a blob whose counter lags its own
        // table (corruption) must not hand out ids that alias stored ones.
        if(id >= self.next_fv_id) {
            self.next_fv_id = id + 1;
        }
    }

    for(std::size_t k = 0; k < blob.manifest_fvs.size(); k += 1) {
        manifest_pins[blob.manifest_fvs[k]] = blob.manifest_gens[k];
    }

    // The blob's bitmap ids are the writing session's pool ids: intern its
    // path table and remap every decoded id into this session's pool. Every
    // id was proven covered by the table above.
    llvm::DenseMap<std::uint32_t, std::uint32_t> remap;
    remap.reserve(blob.sym_paths.size());
    for(auto& [id, path]: blob.sym_paths) {
        remap.try_emplace(id, pool.intern(path));
    }

    self.symbols.reserve(sym_count);
    for(std::size_t k = 0; k < sym_count; k += 1) {
        Bitmap remapped;
        for(auto id: bitmaps[k]) {
            remapped.add(remap.find(id)->second);
        }
        auto& symbol = self.symbols[blob.sym_hashes[k]];
        symbol.name = std::move(blob.sym_names[k]);
        symbol.kind = SymbolKind(blob.sym_kinds[k]);
        symbol.reference_files = std::move(remapped);
    }

    return true;
}

}  // namespace clice::index
