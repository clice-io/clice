module clice;

import :feature.document_links;
import :index.format;
import :index.indexing.tu_index;
import :support.text;
import :syntax.token;
import :vfs.file_system;
import :worker.preamble_index;

namespace clice::worker {

namespace {

template <template <typename> typename Seq>
struct LinkOf {
    LocalSourceRange range;
    Seq<char> target;
};

/// The state before the TU index in a `.pch.idx`.
template <template <typename> typename Seq>
struct PreambleStateOf {
    /// index::index_format_version: the TU index after it is read by the
    /// same build.
    std::uint32_t format_version = 0;
    std::uint64_t text_hash = 0;
    std::uint32_t text_size = 0;
    Seq<LinkOf<Seq>> links;
    Seq<std::uint32_t> inactive_regions;
    Seq<std::uint8_t> open_conditionals;
    Seq<char> diagnostics;
};

using PreambleState = PreambleStateOf<codec::blob::Owned>;

/// The file: the state's size in the first `header` bytes, the state, then
/// the TU index, each starting where a blob is read in place.
constexpr std::size_t header = std::max<std::size_t>(codec::blob::alignment, sizeof(std::uint64_t));

std::size_t aligned(std::size_t offset) {
    return (offset + codec::blob::alignment - 1) / codec::blob::alignment * codec::blob::alignment;
}

}  // namespace

std::vector<std::byte> encode_preamble_index(llvm::StringRef preamble_text,
                                             llvm::ArrayRef<std::byte> index,
                                             llvm::ArrayRef<feature::DocumentLink> links,
                                             llvm::ArrayRef<std::uint32_t> inactive_regions,
                                             llvm::ArrayRef<std::uint8_t> open_conditionals,
                                             llvm::StringRef diagnostics) {
    PreambleState state{
        .format_version = index::index_format_version,
        .text_hash = llvm::xxh3_64bits(preamble_text),
        .text_size = static_cast<std::uint32_t>(preamble_text.size()),
        .inactive_regions = inactive_regions.vec(),
        .open_conditionals = open_conditionals.vec(),
        .diagnostics = diagnostics.str(),
    };
    for(auto& link: links) {
        state.links.push_back({.range = link.range, .target = link.target});
    }
    auto encoded = index::encode(state);

    std::vector<std::byte> bytes(aligned(header + encoded.size()));
    std::uint64_t size = encoded.size();
    std::memcpy(bytes.data(), &size, sizeof(size));
    std::memcpy(bytes.data() + header, encoded.data(), encoded.size());
    bytes.insert(bytes.end(), index.begin(), index.end());
    return bytes;
}

std::shared_ptr<PreambleIndex> open_preamble_index(llvm::StringRef path, bool verified) {
    auto file = fs::File::open(path);
    if(!file) {
        return nullptr;
    }
    auto buffer = llvm::MemoryBuffer::getOpenFile(file->native_handle(),
                                                  path,
                                                  /*FileSize=*/-1,
                                                  /*RequiresNullTerminator=*/false,
                                                  /*IsVolatile=*/false);
    if(!buffer) {
        return nullptr;
    }
    // A stale or truncated pair must never crash the server: both parts are
    // deep-verified, and every embedded shard blob once — queries then run
    // unchecked. Anything failing reads as "pair missing" and the PCH is
    // rebuilt.
    auto bytes = (*buffer)->getBuffer();
    std::uint64_t size = 0;
    if(bytes.size() < header) {
        return nullptr;
    }
    std::memcpy(&size, bytes.data(), sizeof(size));
    if(size > bytes.size() - header) {
        return nullptr;
    }
    auto* state = index::verify_root<PreambleState>(bytes.substr(header, size), "preamble state");
    if(!state) {
        return nullptr;
    }
    auto result = std::make_shared<PreambleIndex>();
    result->index = index::TUIndex::from_bytes(bytes.substr(aligned(header + size)));
    if(!result->index.loaded() || (!verified && !result->index.verify_shards())) {
        return nullptr;
    }
    result->text_hash = state->text_hash;
    result->text_size = state->text_size;
    for(std::uint32_t i = 0; i < state->links.size(); i += 1) {
        auto& link = state->links[i];
        result->links.push_back({.range = link.range, .target = std::string(link.target)});
    }
    result->inactive_regions = llvm::ArrayRef<std::uint32_t>(state->inactive_regions).vec();
    result->open_conditionals = llvm::ArrayRef<std::uint8_t>(state->open_conditionals).vec();
    result->diagnostics = std::string(llvm::StringRef(state->diagnostics));
    result->mapped = std::move(*buffer);
    return result;
}

}  // namespace clice::worker
