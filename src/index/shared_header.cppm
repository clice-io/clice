module clice:index.shared_header;

import :compile.shared_header;
import :index.format;
import :index.symbol;
import :support.prelude;

namespace clice::index {

/// What the build of a shared header learned (SharedHeaderRecord), with each
/// file's section in the shared header's own TU index: a unit loading the
/// PCH sends the variant of each held header it includes by its bare hash,
/// as it would a variant it found known. The cache stores it beside the PCH.
struct SharedHeaderIndex {
    SharedHeaderRecord record;

    /// Per file of the record, its section: variant 0 for a file the shared
    /// header's index left without entries.
    std::vector<KnownIdentity> sections;

    /// Per file of the record, the keys of the instantiations the shared
    /// header's index walked or skipped there, ascending: a unit loading
    /// the PCH uses what they resolve as a skipped key's.
    std::vector<std::vector<std::uint64_t>> instantiations;
};

/// `record`, of a shared header's build, with the sections of `tu_index`,
/// the build's TU index.
SharedHeaderIndex shared_header_index(const SharedHeaderRecord& record, llvm::StringRef tu_index);

std::vector<std::byte> encode_shared_header(const SharedHeaderIndex& index);

/// Nullopt for bytes of another format version or that do not verify.
std::optional<SharedHeaderIndex> decode_shared_header(llvm::StringRef bytes);

}  // namespace clice::index
