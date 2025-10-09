#include "Support/Compare.h"
#include "schema_generated.h"
#include "Index/MergedIndex.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_os_ostream.h"

namespace clice::index {

namespace {

auto sha256_hash(FileIndex& index) {
    llvm::SHA256 hasher;

    using u8 = std::uint8_t;

    if(!index.occurrences.empty()) {
        static_assert(sizeof(Occurrence) == sizeof(Range) + sizeof(SymbolHash));
        static_assert(sizeof(Occurrence) % 8 == 0);
        auto data = reinterpret_cast<u8*>(index.occurrences.data());
        auto size = index.occurrences.size() * sizeof(Occurrence);
        hasher.update(llvm::ArrayRef(data, size));
    }

    for(auto& [symbol_id, relations]: index.relations) {
        hasher.update(std::bit_cast<std::array<u8, sizeof(symbol_id)>>(symbol_id));
        static_assert(sizeof(Relation) ==
                      sizeof(RelationKind) + 4 + sizeof(Range) + sizeof(SymbolHash));
        static_assert(sizeof(Relation) % 8 == 0);

        if(!relations.empty()) {
            auto data = reinterpret_cast<u8*>(relations.data());
            auto size = relations.size() * sizeof(Relation);
            hasher.update(llvm::ArrayRef(data, size));
        }
    }

    return hasher.final();
}

}  // namespace

void MergedIndex::remove(llvm::StringRef path) {
    auto& includes = contexts[path].includes;

    for(auto& [_, canonical_id]: includes) {
        auto& ref_counts = canonical_ref_counts[canonical_id];
        ref_counts -= 1;

        if(ref_counts == 0) {
            removed.add(canonical_id);
        }
    }

    includes.clear();
}

void MergedIndex::merge(llvm::StringRef path, std::uint32_t include, FileIndex& index) {
    auto& context = contexts[path];

    auto hash = sha256_hash(index);
    auto hash_key = llvm::StringRef(reinterpret_cast<char*>(hash.data()), hash.size());
    auto [it, success] = canonical_cache.try_emplace(hash_key, max_canonical_id);

    auto canonical_id = it->second;
    context.includes.emplace_back(include, canonical_id);

    if(!success) {
        canonical_ref_counts[canonical_id] += 1;
        removed.remove(canonical_id);
        return;
    }

    for(auto& occurrence: index.occurrences) {
        this->occurrences[occurrence].add(canonical_id);
    }

    for(auto& [symbol_id, relations]: index.relations) {
        auto& target = this->relations[symbol_id];
        for(auto& relation: relations) {
            target[relation].add(canonical_id);
        }
    }

    canonical_ref_counts.emplace_back(1);
    max_canonical_id += 1;
}

std::vector<Occurrence> MergedIndex::lookup(std::uint32_t offset) {
    if(cache_occurrences.size() != occurrences.size()) {
        cache_occurrences.clear();
        for(auto& [occurrence, _]: occurrences) {
            cache_occurrences.emplace_back(occurrence);
        }
        std::ranges::sort(cache_occurrences, refl::less);
    }

    auto it =
        std::ranges::lower_bound(cache_occurrences, offset, {}, [](index::Occurrence& occurrence) {
            return occurrence.range.end;
        });

    std::vector<index::Occurrence> occurrences;
    while(it != cache_occurrences.end()) {
        if(it->range.contains(offset)) {
            occurrences.emplace_back(*it);
            it++;
            continue;
        }

        break;
    }

    return occurrences;
}

}  // namespace clice::index
