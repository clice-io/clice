module;

#include "support/prelude.h"

module clice;

import :index.columns;
import :index.format;
import :index.project_records;
import :support.bytes;
import :unittests.utils;

namespace clice::testing {
namespace {

Bitmap ids(std::initializer_list<std::uint32_t> values) {
    Bitmap set;
    for(auto value: values) {
        set.add(value);
    }
    return set;
}

/// `blob`'s bytes in aligned storage, as a database read hands them out.
std::unique_ptr<llvm::MemoryBuffer> aligned(const std::vector<std::byte>& blob) {
    return llvm::MemoryBuffer::getMemBufferCopy(as_chars(blob));
}

ZEST_SUITE(ProjectRecords) {

ZEST_CASE(FileSetForms) {
    // A set is its ids' distances as varints, unless its roaring image is
    // shorter; each reads back, and owns its ids: the bytes are gone before
    // it is compared.
    Bitmap dense;
    dense.addRange(0, 10000);
    for(auto set: {ids({}), ids({7}), ids({3, 9}), ids({1, 2, 3, 100000}), dense}) {
        std::optional<Bitmap> read;
        {
            std::vector<std::uint8_t> bytes;
            index::append_file_set(bytes, set);
            read = index::read_file_set(bytes);
            ranges::fill(bytes, 0xff);
        }
        ZASSERT(read);
        ZEXPECT(*read == set);
    }
    std::vector<std::uint8_t> image;
    index::append_file_set(image, dense);
    ZEXPECT(image.front() == 0);

    // One set, one encoding: no zero distance, no list where the image is
    // shorter, no image where the list is.
    ZEXPECT(!index::read_file_set(std::vector<std::uint8_t>{4, 0}));
    std::vector<std::uint8_t> wrapping;
    index::write_varint(wrapping, (std::uint64_t(1) << 32) + 5);
    ZEXPECT(!index::read_file_set(wrapping));
    ZEXPECT(!index::read_file_set(std::vector<std::uint8_t>(10000, 1)));
    auto small = index::write_bitmap(ids({3, 9}));
    std::vector<std::uint8_t> small_image = {0};
    for(auto byte: small) {
        small_image.push_back(static_cast<std::uint8_t>(byte));
    }
    ZEXPECT(!index::read_file_set(small_image));
}

ZEST_CASE(HeaderRoundTrip) {
    index::Header header{
        .next_path = 12,
        .next_version = 30,
        .search_generation = 3,
        .search_pending = {5, 8},
        .search_pending_files = {2, 11},
        .owed_units = {4},
    };
    auto buffer = aligned(index::encode_record(header));
    auto read = index::decode_header(buffer->getBuffer());
    ZASSERT(read);
    ZEXPECT(read->next_path == 12);
    ZEXPECT(read->next_version == 30);
    ZEXPECT(read->search_generation == 3);
    ZEXPECT(read->search_pending == header.search_pending);
    ZEXPECT(read->search_pending_files == header.search_pending_files);
    ZEXPECT(read->owed_units == header.owed_units);

    // A pending file past the path ids handed out is no header, nor is a
    // pending symbol of a reserved hash.
    header.search_pending_files = {2, 12};
    auto past = aligned(index::encode_record(header));
    ZEXPECT(!index::decode_header(past->getBuffer()));
    header.search_pending_files = {2, 11};
    header.search_pending = {5, ~std::uint64_t(0)};
    auto reserved = aligned(index::encode_record(header));
    ZEXPECT(!index::decode_header(reserved->getBuffer()));
}

ZEST_CASE(FileRoundTrip) {
    index::FileRecord file{
        .path = 4,
        .absent = true,
        .consumed = {10,                                            20 },
        .generation = 20,
        .stored = {111,                                           222},
        .run = 1700000000000,
        .versions = {{.content_hash = 10, .id = 3, .manifests = 2},
                     {.content_hash = 20, .id = 7, .manifests = 1}     },
    };
    auto buffer = aligned(index::encode_record(file));
    auto read = index::decode_file(buffer->getBuffer());
    ZASSERT(read);
    ZEXPECT(*read == file);

    // Versions out of order, or one no manifest names, are no file record.
    auto versions = file.versions;
    ranges::reverse(file.versions);
    auto reversed = aligned(index::encode_record(file));
    ZEXPECT(!index::decode_file(reversed->getBuffer()));
    file.versions = versions;
    file.versions[1].manifests = 0;
    auto unnamed = aligned(index::encode_record(file));
    ZEXPECT(!index::decode_file(unnamed->getBuffer()));
    file.versions = versions;

    // A shard generation without the variants it stores, or the other way
    // round, is no file record; nor are consumed hashes out of order.
    file.stored.clear();
    auto lacking = aligned(index::encode_record(file));
    ZEXPECT(!index::decode_file(lacking->getBuffer()));
    file.generation = 0;
    file.stored = {111};
    auto ungenerated = aligned(index::encode_record(file));
    ZEXPECT(!index::decode_file(ungenerated->getBuffer()));
    file.generation = 20;
    file.consumed = {20, 10};
    auto unordered = aligned(index::encode_record(file));
    ZEXPECT(!index::decode_file(unordered->getBuffer()));
}

ZEST_CASE(VersionRoundTrip) {
    index::PersistedVersion version{.path = 70000, .content_hash = 0xfeedfacecafebeefull};
    auto bytes = index::encode_record(version);
    auto read = index::decode_version(as_chars(bytes));
    ZASSERT(read);
    ZEXPECT(*read == version);

    bytes.pop_back();
    ZEXPECT(!index::decode_version(as_chars(bytes)));
}

ZEST_CASE(DependentsRoundTrip) {
    index::Dependents dependents;
    index::add_unit(dependents.readers, 20, 7);
    index::add_unit(dependents.readers, 10, 3);
    index::add_unit(dependents.readers, 10, 9);
    index::add_unit(dependents.contributors, 0xfeed, 3);
    dependents.absent = ids({5});
    index::add_unit(dependents.importers, 30, 8);
    index::add_unit(dependents.uses, 0xbeef, 7);
    index::IncludeResolution resolution{
        .content = 10,
        .edges = {{1, 4}, {6, 5}}
    };
    resolution.units = ids({3, 9});
    dependents.resolutions.push_back(std::move(resolution));
    auto buffer = aligned(index::encode_record(dependents));
    auto read = index::decode_dependents(buffer->getBuffer());
    ZASSERT(read);
    ZEXPECT(read->readers == dependents.readers);
    ZEXPECT(read->contributors == dependents.contributors);
    ZEXPECT(read->absent == dependents.absent);
    ZEXPECT(read->importers == dependents.importers);
    ZEXPECT(read->uses == dependents.uses);
    ZASSERT(read->resolutions.size() == std::size_t(1));
    ZEXPECT(read->resolutions[0].edges == dependents.resolutions[0].edges);
    ZEXPECT(read->resolutions[0].units == dependents.resolutions[0].units);

    // The last unit leaving a key takes the key with it.
    index::remove_unit(dependents.readers, 20, 7);
    ZEXPECT(dependents.readers.size() == std::size_t(1));
}

ZEST_CASE(SymbolRoundTrip) {
    index::Symbol symbol{
        .name = "find",
        .args = "<int>",
        .signature = "(char c) const",
        .parent = 0x0123456789abcdef,
        .kind = SymbolKind::Method,
        .flags = index::SymbolFlags::HasDefinition,
        .token_class = {.kind = SemanticTokenKind::Method, .modifiers = 3},
        .file = 2,
    };
    symbol.reference_files = ids({2, 5, 9, 400});
    symbol.instantiation_files = ids({5});
    auto buffer = aligned(index::encode_record(symbol));
    auto read = index::decode_symbol(buffer->getBuffer());
    ZASSERT(read);
    ZEXPECT(*read == symbol);

    // Arguments past 64 KiB, as a deep expression template spells them.
    symbol.args = "<" + std::string(70000, 'T') + ">";
    auto wide = aligned(index::encode_record(symbol));
    read = index::decode_symbol(wide->getBuffer());
    ZASSERT(read);
    ZEXPECT(*read == symbol);

    // A symbol no file declares and no instantiation names.
    symbol.file = index::no_file;
    symbol.instantiation_files = {};
    auto bare = aligned(index::encode_record(symbol));
    read = index::decode_symbol(bare->getBuffer());
    ZASSERT(read);
    ZEXPECT(*read == symbol);
}

ZEST_CASE(SymbolBytesChecked) {
    index::Symbol symbol{.name = "name", .file = index::no_file};
    auto bytes = index::encode_record(symbol);
    auto decode = [&] {
        return index::decode_symbol(as_chars(bytes));
    };
    ZASSERT(decode());

    // The record: the three texts (5 + 1 + 1 bytes), the parent (8), the
    // file, the modifiers and the flags (a byte each), the kind, the token
    // kind and the reference files' size.
    ZASSERT(bytes.size() == std::size_t(21));
    bytes[19] = std::byte(0xff);
    ZEXPECT(!decode());
    bytes[19] = std::byte(0);
    bytes[0] = std::byte(30);
    ZEXPECT(!decode());
    bytes[0] = std::byte(4);

    // The file, the modifiers and the flags past their widths.
    auto with = [&](std::uint32_t at, std::uint64_t value) {
        std::vector<std::uint8_t> varint;
        index::write_varint(varint, value);
        auto changed = bytes;
        changed.erase(changed.begin() + at);
        for(auto byte: views::reverse(varint)) {
            changed.insert(changed.begin() + at, std::byte(byte));
        }
        return index::decode_symbol(as_chars(changed));
    };
    ZASSERT(with(15, index::no_file));
    ZEXPECT(!with(15, std::uint64_t(index::no_file) + 1));
    ZEXPECT(!with(16, std::uint64_t(1) << 32));
    ZEXPECT(!with(17, 0x10000));

    // Cut inside the parent, before the token kind, or before the reference
    // files' size.
    for(std::size_t size: {10, 19, 20}) {
        auto cut = bytes;
        cut.resize(size);
        ZEXPECT(!index::decode_symbol(as_chars(cut)));
    }
}

ZEST_CASE(InternalsRoundTrip) {
    index::Internals internals = {
        {.hash = 3, .name = "helper", .args = "",    .parent = 0, .kind = SymbolKind::Function},
        {.hash = 9, .name = "Hidden", .args = "<T>", .parent = 4, .kind = SymbolKind::Class   },
    };
    auto buffer = aligned(index::encode_record(internals));
    auto read = index::decode_internals(buffer->getBuffer());
    ZASSERT(read);
    ZASSERT(read->size() == std::size_t(2));
    ZEXPECT((*read)[1].name == "Hidden");
    ZEXPECT((*read)[1].args == "<T>");
    ZEXPECT((*read)[1].parent == 4u);
}

ZEST_CASE(SharedLocalRoundTrip) {
    index::SharedLocalSets sets = {
        {{1, 4}, 2},
        {{3},    1},
    };
    auto buffer = aligned(index::encode_record(sets));
    auto read = index::decode_shared_local(buffer->getBuffer());
    ZASSERT(read);
    ZEXPECT(*read == sets);

    // A set no unit lists is no record, nor are sets out of order.
    sets[1].second = 0;
    auto unlisted = aligned(index::encode_record(sets));
    ZEXPECT(!index::decode_shared_local(unlisted->getBuffer()));
    sets[1].second = 1;
    std::swap(sets[0], sets[1]);
    auto unordered = aligned(index::encode_record(sets));
    ZEXPECT(!index::decode_shared_local(unordered->getBuffer()));
}

};  // ZEST_SUITE(ProjectRecords)

}  // namespace
}  // namespace clice::testing
