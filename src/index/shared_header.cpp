module clice;

import :compile.shared_header;
import :index.format;
import :index.shared_header;
import :index.tu_index;

namespace clice::index {

namespace {

template <template <typename> typename Seq>
struct SharedHeaderFileOf {
    Seq<char> path;

    std::uint64_t content_hash = 0;

    std::uint64_t variant = 0;

    std::uint64_t identity = 0;

    std::uint64_t own_identity = 0;

    std::uint8_t reach = 0;
};

template <template <typename> typename Seq>
struct SharedHeaderHeaderOf {
    std::uint32_t file = 0;

    std::uint8_t system = 0;

    Seq<MacroState> reads;

    Seq<MacroState> writes;

    Seq<SharedHeaderRecord::Include> includes;
};

template <template <typename> typename Seq>
struct SharedHeaderWrittenOf {
    std::uint64_t name = 0;

    Seq<char> spelling;

    std::uint64_t initial = 0;

    std::uint8_t project = 0;
};

/// The layout of SharedHeaderIndex.
template <template <typename> typename Seq>
struct SharedHeaderBlobOf {
    std::uint32_t format_version = 0;

    Seq<SharedHeaderFileOf<Seq>> files;

    Seq<SharedHeaderHeaderOf<Seq>> headers;

    Seq<std::uint32_t> forced;

    Seq<SharedHeaderWrittenOf<Seq>> written;

    Seq<MacroState> initial;
};

using SharedHeaderBlob = SharedHeaderBlobOf<codec::blob::Owned>;

template <typename T, typename Seq>
std::vector<T> copy(const Seq& seq) {
    return std::vector<T>(seq.begin(), seq.end());
}

}  // namespace

SharedHeaderIndex shared_header_index(const SharedHeaderRecord& record, llvm::StringRef tu_index) {
    SharedHeaderIndex index{.record = record, .sections = {}};
    index.sections.resize(record.files.size());
    auto view = TUIndex::from_bytes(tu_index);
    if(!view.loaded()) {
        return index;
    }
    llvm::StringMap<std::uint32_t> files;
    for(auto [i, file]: views::enumerate(record.files)) {
        files.try_emplace(file.path, static_cast<std::uint32_t>(i));
    }
    for(std::uint32_t i = 0; i < view.section_count(); i += 1) {
        if(auto it = files.find(view.path(view.section_path(i))); it != files.end()) {
            index.sections[it->second] = view.section_identity(i);
            index.sections[it->second].variant = view.section_hash(i);
        }
    }
    return index;
}

std::vector<std::byte> encode_shared_header(const SharedHeaderIndex& index) {
    auto& record = index.record;
    SharedHeaderBlob blob;
    blob.format_version = index_format_version;
    for(auto [file, section]: views::zip(record.files, index.sections)) {
        blob.files.push_back({.path = file.path,
                              .content_hash = file.content_hash,
                              .variant = section.variant,
                              .identity = section.identity,
                              .own_identity = section.own,
                              .reach = section.reach});
    }
    for(auto& header: record.headers) {
        blob.headers.push_back({.file = header.file,
                                .system = header.system,
                                .reads = header.reads,
                                .writes = header.writes,
                                .includes = header.includes});
    }
    blob.forced = record.forced;
    for(auto& written: record.written) {
        blob.written.push_back({.name = written.name,
                                .spelling = written.spelling,
                                .initial = written.initial,
                                .project = written.project});
    }
    blob.initial = record.initial;
    return encode(blob);
}

std::optional<SharedHeaderIndex> decode_shared_header(llvm::StringRef bytes) {
    auto* root = verify_root<SharedHeaderBlob>(bytes, "a shared header's record");
    if(!root) {
        return std::nullopt;
    }
    SharedHeaderIndex index;
    auto& record = index.record;
    auto files = root->files.size();
    for(auto& file: root->files) {
        record.files.push_back({.path = llvm::StringRef(file.path).str(),
                                .content_hash = file.content_hash});
        index.sections.push_back({.identity = file.identity,
                                  .own = file.own_identity,
                                  .reach = file.reach,
                                  .variant = file.variant});
    }
    auto headers = root->headers.size();
    for(auto& header: root->headers) {
        if(header.file >= files) {
            return std::nullopt;
        }
        auto& into = record.headers.emplace_back();
        into.file = header.file;
        into.system = header.system != 0;
        into.reads = copy<MacroState>(header.reads);
        into.writes = copy<MacroState>(header.writes);
        into.includes = copy<SharedHeaderRecord::Include>(header.includes);
        for(auto [i, include]: views::enumerate(into.includes)) {
            if(include.file >= files || include.writes > into.writes.size() ||
               (include.held != SharedHeaderRecord::none && include.held >= headers) ||
               (include.parent != SharedHeaderRecord::none &&
                include.parent >= static_cast<std::uint32_t>(i))) {
                return std::nullopt;
            }
        }
    }
    record.forced = copy<std::uint32_t>(root->forced);
    if(ranges::any_of(record.forced, [&](std::uint32_t header) { return header >= headers; })) {
        return std::nullopt;
    }
    for(auto& written: root->written) {
        record.written.push_back({.name = written.name,
                                  .spelling = llvm::StringRef(written.spelling).str(),
                                  .initial = written.initial,
                                  .project = written.project != 0});
    }
    record.initial = copy<MacroState>(root->initial);
    if(!ranges::is_sorted(record.written, {}, &SharedHeaderRecord::Written::name) ||
       !ranges::is_sorted(record.initial, {}, &MacroState::name)) {
        return std::nullopt;
    }
    return index;
}

}  // namespace clice::index
