module clice;

import :index.columns;
import :index.format;
import :index.project_records;
import :semantic.token_class;
import :support.bytes;
import :support.logging;

namespace clice::index {

namespace {

template <typename Blob>
const codec::blob::view_t<Blob>* view_record(llvm::StringRef data) {
    auto root = codec::blob::view<Blob>(as_bytes(data));
    return root ? *root : nullptr;
}

/// The size of `ids` as a list of varint distances (append_file_set).
std::uint64_t list_size(const Bitmap& ids) {
    std::uint64_t size = 0;
    std::int64_t previous = -1;
    for(std::uint32_t id: ids) {
        for(auto distance = std::uint64_t(id - previous); distance >= 0x80; distance >>= 7) {
            size += 1;
        }
        size += 1;
        previous = id;
    }
    return size;
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

void encode_units(const KeyedUnits& sets, std::vector<UnitSetOf<codec::blob::Owned>>& out) {
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
    auto image_size = ids.getSizeInBytes(true);
    if(list_size(ids) <= 1 + image_size) {
        std::int64_t previous = -1;
        for(std::uint32_t id: ids) {
            write_varint(arena, std::uint64_t(id - previous));
            previous = id;
        }
        return;
    }
    arena.push_back(0);
    auto image = write_bitmap(ids);
    auto* bytes = reinterpret_cast<const std::uint8_t*>(image.data());
    arena.insert(arena.end(), bytes, bytes + image.size());
}

std::optional<Bitmap> read_file_set(llvm::ArrayRef<std::uint8_t> bytes) {
    Bitmap ids;
    if(!bytes.empty() && bytes.front() == 0) {
        auto image = read_bitmap(bytes.data() + 1, bytes.size() - 1);
        if(!image) {
            return std::nullopt;
        }
        // read_bitmap's view borrows `bytes`, which outlive no decode.
        ids = Bitmap(*image);
        if(ids.getSizeInBytes(true) != bytes.size() - 1 || list_size(ids) <= bytes.size()) {
            return std::nullopt;
        }
        return ids;
    }
    std::int64_t previous = -1;
    std::uint32_t at = 0;
    while(at < bytes.size()) {
        auto distance = read_varint(bytes, at);
        if(!distance || *distance == 0 ||
           *distance > std::numeric_limits<std::uint32_t>::max() - previous) {
            return std::nullopt;
        }
        previous += static_cast<std::int64_t>(*distance);
        ids.add(static_cast<std::uint32_t>(previous));
    }
    if(bytes.size() > 1 + ids.getSizeInBytes(true)) {
        return std::nullopt;
    }
    return ids;
}

void add_unit(KeyedUnits& sets, std::uint64_t key, std::uint32_t unit) {
    auto it = ranges::lower_bound(sets, key, {}, &KeyedUnits::value_type::first);
    if(it == sets.end() || it->first != key) {
        it = sets.emplace(it, key, Bitmap());
    }
    it->second.add(unit);
}

void remove_unit(KeyedUnits& sets, std::uint64_t key, std::uint32_t unit) {
    auto it = ranges::lower_bound(sets, key, {}, &KeyedUnits::value_type::first);
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
    blob.next_version = header.next_version;
    blob.unit_count = header.unit_count;
    blob.search_segments = header.search_segments;
    blob.search_run_ends = header.search_run_ends;
    blob.next_segment = header.next_segment;
    blob.search_pending = header.search_pending;
    blob.search_pending_files = header.search_pending_files;
    blob.owed_units = header.owed_units;
    return encode(blob);
}

std::optional<Header> decode_header(llvm::StringRef data) {
    auto* root = verify_root<HeaderBlob>(data, "index database header");
    auto named = [&](const auto& paths) {
        return strictly_ascending(paths) &&
               ranges::all_of(paths, [&](std::uint32_t path) { return path < root->next_path; });
    };
    // Each run holds a segment at least, and the runs end at the last one.
    auto runs_ok = [&] {
        auto& segments = root->search_segments;
        auto& ends = root->search_run_ends;
        if(segments.empty() || ends.empty()) {
            return segments.empty() && ends.empty();
        }
        return strictly_ascending(segments) && segments.back() < root->next_segment &&
               strictly_ascending(ends) && ends.front() > 0 && ends.back() == segments.size();
    };
    if(!root || root->unit_count > root->next_path || !runs_ok() ||
       !strictly_ascending(root->search_pending) ||
       ranges::any_of(root->search_pending, [](SymbolHash hash) { return reserved_key(hash); }) ||
       !named(root->search_pending_files) || !named(root->owed_units)) {
        return std::nullopt;
    }
    return Header{
        .next_path = root->next_path,
        .next_version = root->next_version,
        .unit_count = root->unit_count,
        .search_segments = {root->search_segments.begin(),      root->search_segments.end()},
        .search_run_ends = {root->search_run_ends.begin(),      root->search_run_ends.end()},
        .next_segment = root->next_segment,
        .search_pending = {root->search_pending.begin(),       root->search_pending.end() },
        .search_pending_files = {root->search_pending_files.begin(),
                            root->search_pending_files.end()                               },
        .owed_units = {root->owed_units.begin(),           root->owed_units.end()     },
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
    for(auto& version: dependents.versions) {
        blob.version_hashes.push_back(version.content_hash);
        blob.version_ids.push_back(version.id);
        blob.version_manifests.push_back(version.manifests);
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
    if(!strictly_ascending(root->version_hashes) ||
       root->version_ids.size() != root->version_hashes.size() ||
       root->version_manifests.size() != root->version_hashes.size() ||
       ranges::contains(root->version_manifests, 0u)) {
        return std::nullopt;
    }
    for(std::uint32_t i = 0; i < root->version_hashes.size(); i += 1) {
        dependents.versions.push_back({
            .content_hash = root->version_hashes[i],
            .id = root->version_ids[i],
            .manifests = root->version_manifests[i],
        });
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
       ranges::any_of(root->hashes, [](SymbolHash hash) { return reserved_key(hash); }) ||
       ranges::any_of(root->parents, [](SymbolHash hash) { return reserved_key(hash); }) ||
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
    std::vector<std::uint8_t> bytes;
    for(llvm::StringRef text: {llvm::StringRef(symbol.name),
                               llvm::StringRef(symbol.args),
                               llvm::StringRef(symbol.signature)}) {
        write_varint(bytes, text.size());
        bytes.insert(bytes.end(), text.bytes_begin(), text.bytes_end());
    }
    for(std::uint32_t shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(symbol.parent >> shift));
    }
    write_varint(bytes, symbol.file == no_file ? 0 : std::uint64_t(symbol.file) + 1);
    write_varint(bytes, symbol.token_class.modifiers);
    write_varint(bytes, static_cast<std::uint16_t>(symbol.flags));
    bytes.push_back(symbol.kind.value());
    bytes.push_back(static_cast<std::uint8_t>(symbol.token_class.kind));
    auto references = file_set(symbol.reference_files);
    write_varint(bytes, references.size());
    bytes.insert(bytes.end(), references.begin(), references.end());
    append_file_set(bytes, symbol.instantiation_files);
    auto* data = reinterpret_cast<const std::byte*>(bytes.data());
    return {data, data + bytes.size()};
}

std::optional<Symbol> decode_symbol(llvm::StringRef data) {
    llvm::ArrayRef<std::uint8_t> bytes(data.bytes_begin(), data.bytes_end());
    std::uint32_t at = 0;
    auto number = [&](std::uint64_t limit) -> std::optional<std::uint64_t> {
        auto value = read_varint(bytes, at);
        if(!value || *value > limit) {
            return std::nullopt;
        }
        return value;
    };
    auto slice = [&]() -> std::optional<llvm::ArrayRef<std::uint8_t>> {
        auto size = read_varint(bytes, at);
        if(!size || *size > bytes.size() - at) {
            return std::nullopt;
        }
        auto part = bytes.slice(at, *size);
        at += static_cast<std::uint32_t>(*size);
        return part;
    };
    auto text = [](llvm::ArrayRef<std::uint8_t> part) {
        return std::string(part.begin(), part.end());
    };
    auto name = slice();
    auto args = name ? slice() : std::nullopt;
    auto signature = args ? slice() : std::nullopt;
    if(!signature || bytes.size() - at < 8) {
        return std::nullopt;
    }
    SymbolHash parent = 0;
    for(std::uint32_t shift = 0; shift < 64; shift += 8) {
        parent |= SymbolHash(bytes[at]) << shift;
        at += 1;
    }
    auto file = number(std::uint64_t(no_file));
    auto modifiers = file ? number(std::numeric_limits<std::uint32_t>::max()) : std::nullopt;
    auto flags = modifiers ? number(std::numeric_limits<std::uint16_t>::max()) : std::nullopt;
    if(!flags || reserved_key(parent) || bytes.size() - at < 2 ||
       !all_token_kinds(bytes.slice(at + 1, 1))) {
        return std::nullopt;
    }
    auto kind = bytes[at];
    auto token_kind = bytes[at + 1];
    at += 2;
    auto reference_bytes = slice();
    if(!reference_bytes) {
        return std::nullopt;
    }
    auto references = read_file_set(*reference_bytes);
    auto instantiations = read_file_set(bytes.drop_front(at));
    if(!references || !instantiations) {
        return std::nullopt;
    }
    return Symbol{
        .name = text(*name),
        .args = text(*args),
        .signature = text(*signature),
        .parent = parent,
        .kind = SymbolKind(kind),
        .flags = static_cast<SymbolFlags>(*flags),
        .token_class = {.kind = static_cast<SemanticTokenKind>(token_kind),
                        .modifiers = static_cast<std::uint32_t>(*modifiers)},
        .file = *file == 0 ? no_file : static_cast<std::uint32_t>(*file - 1),
        .reference_files = std::move(*references),
        .instantiation_files = std::move(*instantiations),
    };
}

void measure_symbol(llvm::StringRef data, FieldBytes& bytes) {
    auto symbol = decode_symbol(data);
    if(!symbol) {
        return;
    }
    auto varints = [](std::initializer_list<std::uint64_t> values) {
        std::vector<std::uint8_t> encoded;
        for(auto value: values) {
            write_varint(encoded, value);
        }
        return encoded.size();
    };
    auto references = file_set(symbol->reference_files).size();
    bytes.add("text",
              varints({symbol->name.size(), symbol->args.size(), symbol->signature.size()}) +
                  symbol->name.size() + symbol->args.size() + symbol->signature.size());
    bytes.add("parent", sizeof(SymbolHash));
    bytes.add("file, modifiers, flags, kinds",
              varints({symbol->file == no_file ? 0 : std::uint64_t(symbol->file) + 1,
                       symbol->token_class.modifiers,
                       static_cast<std::uint16_t>(symbol->flags)}) +
                  2);
    bytes.add("reference files", varints({references}) + references);
    bytes.add("instantiation files", file_set(symbol->instantiation_files).size());
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

std::vector<std::byte> encode_record(const PersistedVersion& version) {
    std::vector<std::byte> bytes(sizeof(version.path) + sizeof(version.content_hash));
    std::memcpy(bytes.data(), &version.path, sizeof(version.path));
    std::memcpy(bytes.data() + sizeof(version.path),
                &version.content_hash,
                sizeof(version.content_hash));
    return bytes;
}

std::optional<PersistedVersion> decode_version(llvm::StringRef data) {
    PersistedVersion version;
    if(data.size() != sizeof(version.path) + sizeof(version.content_hash)) {
        return std::nullopt;
    }
    std::memcpy(&version.path, data.data(), sizeof(version.path));
    std::memcpy(&version.content_hash,
                data.data() + sizeof(version.path),
                sizeof(version.content_hash));
    return version;
}

}  // namespace clice::index
