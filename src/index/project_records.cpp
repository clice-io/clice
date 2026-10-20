module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.columns;
import :index.format;
import :index.project_records;
import :semantic.token_class;
import :support.bytes;
import :support.logging;

namespace clice::index {

namespace {

/// Most file sets hold one or two files: up to raw_file_set_limit path ids
/// are stored raw as u32s, more as a portable roaring image — which is
/// longer than any raw list, so a slice's length says which it is.
constexpr std::uint32_t raw_file_set_limit = 2;

template <typename Blob>
const kota::codec::blob::view_t<Blob>* view_record(llvm::StringRef data) {
    auto root = kota::codec::blob::view<Blob>(as_bytes(data));
    return root ? *root : nullptr;
}

std::vector<std::uint8_t> file_set(const Bitmap& ids) {
    std::vector<std::uint8_t> bytes;
    append_file_set(bytes, ids);
    return bytes;
}

template <typename Seq>
std::optional<Bitmap> file_set_of(const Seq& bytes) {
    return read_file_set(llvm::ArrayRef<std::uint8_t>(bytes.data(), bytes.size()));
}

void encode_units(const KeyedUnits& sets, std::vector<UnitSetOf<kota::codec::blob::Owned>>& out) {
    for(auto& [key, units]: sets) {
        out.push_back({.key = key, .units = file_set(units)});
    }
}

template <typename Seq>
std::optional<KeyedUnits> decode_units(const Seq& sets) {
    KeyedUnits decoded;
    for(auto& set: sets) {
        auto units = file_set_of(set.units);
        if(!units || units->isEmpty() || (!decoded.empty() && set.key <= decoded.back().first)) {
            return std::nullopt;
        }
        decoded.emplace_back(set.key, std::move(*units));
    }
    return decoded;
}

}  // namespace

void append_file_set(std::vector<std::uint8_t>& arena, const Bitmap& ids) {
    if(ids.cardinality() <= raw_file_set_limit) {
        for(std::uint32_t id: ids) {
            auto* bytes = reinterpret_cast<const std::uint8_t*>(&id);
            arena.insert(arena.end(), bytes, bytes + sizeof(id));
        }
        return;
    }
    auto image = write_bitmap(ids);
    auto* bytes = reinterpret_cast<const std::uint8_t*>(image.data());
    arena.insert(arena.end(), bytes, bytes + image.size());
}

std::optional<Bitmap> read_file_set(llvm::ArrayRef<std::uint8_t> bytes) {
    if(bytes.size() > raw_file_set_limit * sizeof(std::uint32_t)) {
        auto image = read_bitmap(bytes.data(), bytes.size());
        if(!image || image->cardinality() <= raw_file_set_limit) {
            return std::nullopt;
        }
        // read_bitmap's view borrows `bytes`, which outlive no decode.
        return Bitmap(*image);
    }
    if(bytes.size() % sizeof(std::uint32_t) != 0) {
        return std::nullopt;
    }
    Bitmap ids;
    std::optional<std::uint32_t> previous;
    for(std::uint32_t i = 0; i < bytes.size(); i += sizeof(std::uint32_t)) {
        std::uint32_t id;
        std::memcpy(&id, bytes.data() + i, sizeof(id));
        if(previous && id <= *previous) {
            return std::nullopt;
        }
        ids.add(id);
        previous = id;
    }
    return ids;
}

void add_unit(KeyedUnits& sets, std::uint64_t key, std::uint32_t unit) {
    auto it = llvm::lower_bound(sets, key, [](const auto& set, std::uint64_t key) {
        return set.first < key;
    });
    if(it == sets.end() || it->first != key) {
        it = sets.emplace(it, key, Bitmap());
    }
    it->second.add(unit);
}

void remove_unit(KeyedUnits& sets, std::uint64_t key, std::uint32_t unit) {
    auto it = llvm::lower_bound(sets, key, [](const auto& set, std::uint64_t key) {
        return set.first < key;
    });
    if(it == sets.end() || it->first != key) {
        return;
    }
    it->second.remove(unit);
    if(it->second.isEmpty()) {
        sets.erase(it);
    }
}

std::vector<std::byte> encode_record(const Header& header) {
    HeaderBlob blob;
    blob.format_version = index_format_version;
    blob.next_path = header.next_path;
    blob.search_generation = header.search_generation;
    blob.search_pending = header.search_pending;
    blob.search_pending_files = header.search_pending_files;
    blob.owed_units = header.owed_units;
    return encode(blob);
}

std::optional<Header> decode_header(llvm::StringRef data) {
    auto* root = verify_root<HeaderBlob>(data, "index database header");
    auto named = [&](const auto& paths) {
        return strictly_ascending(paths) &&
               llvm::all_of(paths, [&](std::uint32_t path) { return path < root->next_path; });
    };
    if(!root || !strictly_ascending(root->search_pending) ||
       llvm::any_of(root->search_pending, [](SymbolHash hash) { return reserved_key(hash); }) ||
       !named(root->search_pending_files) || !named(root->owed_units)) {
        return std::nullopt;
    }
    return Header{
        .next_path = root->next_path,
        .search_generation = root->search_generation,
        .search_pending = {root->search_pending.begin(),       root->search_pending.end()},
        .search_pending_files = {root->search_pending_files.begin(),
                           root->search_pending_files.end()                              },
        .owed_units = {root->owed_units.begin(),           root->owed_units.end()    },
    };
}

std::vector<std::byte> encode_record(const FileRecord& file) {
    FileBlob blob;
    blob.path = file.path;
    blob.absent = file.absent;
    blob.consumed = file.consumed;
    blob.generation = file.generation;
    blob.stored = file.stored;
    blob.run = file.run;
    return encode(blob);
}

std::optional<FileRecord> decode_file(llvm::StringRef data) {
    auto* root = view_record<FileBlob>(data);
    if(!root || !strictly_ascending(root->consumed) ||
       (root->generation == 0) != root->stored.empty()) {
        return std::nullopt;
    }
    return FileRecord{
        .path = root->path,
        .absent = root->absent,
        .consumed = {root->consumed.begin(), root->consumed.end()},
        .generation = root->generation,
        .stored = {root->stored.begin(),   root->stored.end()  },
        .run = root->run,
    };
}

std::vector<std::byte> encode_record(const Dependents& dependents) {
    DependentsBlob blob;
    encode_units(dependents.readers, blob.readers);
    encode_units(dependents.contributors, blob.contributors);
    blob.absent = file_set(dependents.absent);
    encode_units(dependents.importers, blob.importers);
    encode_units(dependents.uses, blob.uses);
    for(auto& resolution: dependents.resolutions) {
        auto& out = blob.resolutions.emplace_back();
        out.content = resolution.content;
        for(auto [line, target]: resolution.edges) {
            out.lines.push_back(line);
            out.targets.push_back(target);
        }
        out.units = file_set(resolution.units);
    }
    return encode(blob);
}

std::optional<Dependents> decode_dependents(llvm::StringRef data) {
    auto* root = view_record<DependentsBlob>(data);
    if(!root) {
        return std::nullopt;
    }
    Dependents dependents;
    auto readers = decode_units(root->readers);
    auto contributors = decode_units(root->contributors);
    auto importers = decode_units(root->importers);
    auto uses = decode_units(root->uses);
    if(!readers || !contributors || !importers || !uses) {
        return std::nullopt;
    }
    dependents.readers = std::move(*readers);
    dependents.contributors = std::move(*contributors);
    dependents.importers = std::move(*importers);
    dependents.uses = std::move(*uses);
    auto absent = file_set_of(root->absent);
    if(!absent) {
        return std::nullopt;
    }
    dependents.absent = std::move(*absent);
    for(auto& resolution: root->resolutions) {
        auto units = file_set_of(resolution.units);
        if(!units || units->isEmpty() || resolution.lines.size() != resolution.targets.size()) {
            return std::nullopt;
        }
        auto& decoded = dependents.resolutions.emplace_back();
        decoded.content = resolution.content;
        for(std::uint32_t i = 0; i < resolution.lines.size(); i += 1) {
            decoded.edges.emplace_back(resolution.lines[i], resolution.targets[i]);
        }
        decoded.units = std::move(*units);
    }
    return dependents;
}

std::vector<std::byte> encode_record(const Internals& internals) {
    InternalsBlob blob;
    for(auto& name: internals) {
        blob.hashes.push_back(name.hash);
        blob.names += name.name;
        blob.name_ends.push_back(static_cast<std::uint32_t>(blob.names.size()));
        blob.args += name.args;
        blob.args_ends.push_back(static_cast<std::uint32_t>(blob.args.size()));
        blob.parents.push_back(name.parent);
        blob.kinds.push_back(name.kind.value());
        blob.flags.push_back(static_cast<std::uint16_t>(name.flags));
    }
    return encode(blob);
}

std::optional<Internals> decode_internals(llvm::StringRef data) {
    auto* root = view_record<InternalsBlob>(data);
    if(!root) {
        return std::nullopt;
    }
    auto count = root->hashes.size();
    if(root->name_ends.size() != count || root->args_ends.size() != count ||
       root->parents.size() != count || root->kinds.size() != count ||
       root->flags.size() != count || !strictly_ascending(root->hashes) ||
       llvm::any_of(root->hashes, [](SymbolHash hash) { return reserved_key(hash); }) ||
       llvm::any_of(root->parents, [](SymbolHash hash) { return reserved_key(hash); }) ||
       !monotone_ends(root->name_ends, root->names.size()) ||
       !monotone_ends(root->args_ends, root->args.size())) {
        return std::nullopt;
    }
    Internals internals;
    internals.reserve(count);
    for(std::uint32_t i = 0; i < count; i += 1) {
        internals.push_back({
            .hash = root->hashes[i],
            .name = slice(root->names, root->name_ends, i).str(),
            .args = slice(root->args, root->args_ends, i).str(),
            .parent = root->parents[i],
            .kind = SymbolKind(root->kinds[i]),
            .flags = static_cast<SymbolFlags>(root->flags[i]),
        });
    }
    return internals;
}

std::vector<std::byte> encode_record(const Symbol& symbol) {
    SymbolBlob blob;
    blob.text = symbol.name + symbol.args + symbol.signature;
    blob.name_size = static_cast<std::uint32_t>(symbol.name.size());
    blob.args_size = static_cast<std::uint32_t>(symbol.args.size());
    blob.parent = symbol.parent;
    blob.file = symbol.file == no_file ? 0 : symbol.file + 1;
    blob.token_modifiers = symbol.token_class.modifiers;
    blob.flags = static_cast<std::uint16_t>(symbol.flags);
    blob.kind = symbol.kind.value();
    blob.token_kind = static_cast<std::uint8_t>(symbol.token_class.kind);
    append_file_set(blob.files, symbol.reference_files);
    blob.references_size = static_cast<std::uint32_t>(blob.files.size());
    append_file_set(blob.files, symbol.instantiation_files);
    return encode(blob);
}

std::optional<Symbol> decode_symbol(llvm::StringRef data) {
    auto* root = view_record<SymbolBlob>(data);
    if(!root || std::uint64_t(root->name_size) + root->args_size > root->text.size() ||
       reserved_key(root->parent) || root->references_size > root->files.size() ||
       !all_token_kinds(llvm::ArrayRef<std::uint8_t>(&root->token_kind, 1))) {
        return std::nullopt;
    }
    llvm::StringRef text = std::string_view(root->text);
    llvm::ArrayRef<std::uint8_t> files(root->files.data(), root->files.size());
    auto references = read_file_set(files.take_front(root->references_size));
    auto instantiations = read_file_set(files.drop_front(root->references_size));
    if(!references || !instantiations) {
        return std::nullopt;
    }
    return Symbol{
        .name = text.take_front(root->name_size).str(),
        .args = text.substr(root->name_size, root->args_size).str(),
        .signature = text.drop_front(root->name_size + root->args_size).str(),
        .parent = root->parent,
        .kind = SymbolKind(root->kind),
        .flags = static_cast<SymbolFlags>(root->flags),
        .token_class = {.kind = static_cast<SemanticTokenKind>(root->token_kind),
                        .modifiers = root->token_modifiers},
        .file = root->file == 0 ? no_file : root->file - 1,
        .reference_files = std::move(*references),
        .instantiation_files = std::move(*instantiations),
    };
}

std::vector<std::byte> encode_record(const SharedLocalSets& local) {
    SharedLocalBlob blob;
    for(auto& [files, units]: local) {
        Bitmap set;
        for(auto file: files) {
            set.add(file);
        }
        blob.sets.push_back({.units = units, .files = file_set(set)});
    }
    return encode(blob);
}

std::optional<SharedLocalSets> decode_shared_local(llvm::StringRef data) {
    auto* root = view_record<SharedLocalBlob>(data);
    if(!root) {
        return std::nullopt;
    }
    SharedLocalSets local;
    for(auto& set: root->sets) {
        auto files = file_set_of(set.files);
        if(!files || files->isEmpty() || set.units == 0) {
            return std::nullopt;
        }
        std::vector<std::uint32_t> listed(files->begin(), files->end());
        if(!local.empty() && listed <= local.back().first) {
            return std::nullopt;
        }
        local.emplace_back(std::move(listed), set.units);
    }
    return local;
}

}  // namespace clice::index
