/// The format version every persisted index blob carries, and what the
/// blob layouts share: encoding one, reading one in place, measuring its
/// fields. Each layout lives beside its reader (`layout.cppm` of shard/,
/// indexing/, project_index/ and search/).

module clice:index.format;

import :support.logging;
import :support.prelude;
import :support.text;

namespace clice::index {

/// On-disk index blob schema version. Every persisted blob carries it as a
/// regular field and every loader discards blobs with a different value; a
/// blob of another layout fails the blob codec's check before that.
/// Bump it whenever a persisted type's reflected layout changes, when
/// symbol-hash semantics change (stored entries keyed by old hashes would
/// silently stop matching newly computed ones), or when the meaning of
/// stored data changes.
constexpr inline std::uint32_t index_format_version = 61;

/// The bytes of `blob`, the writer form of a blob layout. A blob holds less
/// than 4 GiB; index blobs stay far below it, so one past it stops the
/// process.
template <typename Blob>
std::vector<std::byte> encode(const Blob& blob) {
    auto bytes = codec::blob::to_bytes(blob);
    if(!bytes) {
        LOG_FATAL("Cannot encode an index blob: {}", bytes.error().to_string());
    }
    return std::move(*bytes);
}

/// Whether `bytes` sit where a blob is read in place: a database or a
/// message hands out bytes at whatever address it keeps them.
inline bool readable_in_place(llvm::StringRef bytes) {
    return reinterpret_cast<std::uintptr_t>(bytes.data()) % codec::blob::alignment == 0;
}

/// `buffer` itself when its bytes are readable_in_place, else an aligned
/// copy of them.
inline std::unique_ptr<llvm::MemoryBuffer>
    in_place_buffer(std::unique_ptr<llvm::MemoryBuffer> buffer) {
    if(buffer && !readable_in_place(buffer->getBuffer())) {
        return llvm::MemoryBuffer::getMemBufferCopy(buffer->getBuffer());
    }
    return buffer;
}

/// The root of a blob of layout `Blob`, read in place, once its bytes pass
/// the structural check and carry this build's format version; null, logged
/// as the rejection of `what`, otherwise. The cross-field checks are the
/// reader's.
template <typename Blob>
const codec::blob::view_t<Blob>* verify_root(llvm::StringRef bytes, llvm::StringRef what) {
    auto root = codec::blob::view<Blob>(as_bytes(bytes));
    if(!root) {
        LOG_DEBUG("Rejecting {}: {}", what, root.error().to_string());
        return nullptr;
    }
    if((*root)->format_version != index_format_version) {
        LOG_DEBUG("Rejecting {}: format version {}, this build reads {}",
                  what,
                  (*root)->format_version,
                  index_format_version);
        return nullptr;
    }
    return *root;
}

/// Swap a reader's backing buffer for byte-identical bytes elsewhere (a
/// newer read snapshot) that are readable_in_place, as a database read
/// gives: what the reader verified and derived describes the
/// bytes, which carry over. False, keeping `buffer`, when the sizes differ.
inline bool rebind_buffer(std::unique_ptr<llvm::MemoryBuffer>& buffer,
                          std::unique_ptr<llvm::MemoryBuffer> replacement) {
    if(!buffer || !replacement || replacement->getBufferSize() != buffer->getBufferSize()) {
        return false;
    }
    buffer = std::move(replacement);
    return true;
}

/// The two highest values of an id type are reserved: they were DenseMap's
/// sentinel keys when the format was defined, so no writer has ever emitted
/// them, and IPC or disk bytes carrying one are corrupt.
template <typename T>
bool reserved_key(T value) {
    static_assert(std::is_unsigned_v<T>);
    return value >= std::numeric_limits<T>::max() - 1;
}

/// The bytes the fields of persisted blobs hold, summed over the blobs
/// measured, by field path (`occurrences.packed`) in declaration order — how
/// `clice index stats` splits a blob kind. Scalar fields, the pairs placing
/// strings and sequences, and padding are left to the framing, the rest of
/// the blobs' size.
struct FieldBytes {
    llvm::SmallVector<std::pair<std::string, std::uint64_t>> fields;

    void add(llvm::StringRef name, std::uint64_t bytes) {
        auto it = ranges::find_if(fields, [&](const auto& field) { return field.first == name; });
        if(it == fields.end()) {
            fields.emplace_back(name.str(), bytes);
        } else {
            it->second += bytes;
        }
    }
};

/// Add the fields of a blob of layout `Blob` to `bytes`; nothing for bytes
/// that do not verify as one.
template <typename Blob>
void measure_blob(llvm::StringRef data, FieldBytes& bytes) {
    if(auto root = codec::blob::view<Blob>(as_bytes(data))) {
        codec::blob::measure(**root, [&](llvm::StringRef path, std::uint64_t size) {
            bytes.add(path, size);
        });
    }
}

}  // namespace clice::index
