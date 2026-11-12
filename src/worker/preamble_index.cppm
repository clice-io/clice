/// What a preamble's build leaves beside its PCH: the `.pch.idx` of the pair.

module clice:worker.preamble_index;

import :feature.document_links;
import :index.indexing.tu_index;
import :support.prelude;

namespace clice::worker {

/// The index of a preamble — every header it covers and the focus file's
/// preamble region — with the state of that region a compile loading the
/// PCH cannot compute again, the PCH having swallowed its directives. The
/// pair is built, stored, hit and evicted together.
struct PreambleIndex {
    /// The mapped file, which `index` reads in place.
    std::unique_ptr<llvm::MemoryBuffer> mapped;

    index::TUIndex index;

    /// xxh3 and size of the exact preamble text the PCH was built from.
    std::uint64_t text_hash = 0;

    std::uint32_t text_size = 0;

    /// The document links of the preamble region.
    std::vector<feature::DocumentLink> links;

    /// The inactive regions within the preamble, flat begin/end offset
    /// pairs.
    std::vector<std::uint32_t> inactive_regions;

    /// The conditional stack still open at the preamble bound.
    std::vector<std::uint8_t> open_conditionals;

    /// The diagnostics the build raised, as it presented them (a JSON
    /// array of feature::FileDiagnostic): a compile loading the PCH never
    /// raises them again.
    std::string diagnostics;

    /// Whether `text` still begins with the exact preamble the pair was
    /// built from: what gates serving any of it against a live buffer,
    /// whose offsets the entries are. Compared by hash, the text itself
    /// is not stored.
    bool matches(llvm::StringRef text) const {
        return text.size() >= text_size &&
               llvm::xxh3_64bits(text.take_front(text_size)) == text_hash;
    }
};

/// The bytes of a `.pch.idx`: the state of the preamble `preamble_text`,
/// then `index`, its TU index (index::build_tu_index).
std::vector<std::byte> encode_preamble_index(llvm::StringRef preamble_text,
                                             llvm::ArrayRef<std::byte> index,
                                             llvm::ArrayRef<feature::DocumentLink> links,
                                             llvm::ArrayRef<std::uint32_t> inactive_regions,
                                             llvm::ArrayRef<std::uint8_t> open_conditionals,
                                             llvm::StringRef diagnostics);

/// Open a `.pch.idx` (memory-mapped). Null when the file is unreadable,
/// structurally invalid, of a different format version, or — unless this
/// process `verified` it whole before, the store's blobs never changing —
/// any embedded shard blob fails verification: callers treat all of these
/// as a PCH cache miss.
std::shared_ptr<PreambleIndex> open_preamble_index(llvm::StringRef path, bool verified = false);

}  // namespace clice::worker
