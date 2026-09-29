#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <system_error>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualFileSystem.h"

namespace clice {

/// One observation of a file's on-disk bytes: the xxh3 of the text a single
/// read returned (see without_bom), and the stat describing the bytes. Captured under
/// the pairing discipline (see vfs::read_observed) so the two halves are
/// same-source: `paired` says the pre/post fstats of the read agreed,
/// `reliable` additionally says the mtime lay outside the filesystem
/// mtime-granularity guard window — only then may the stat serve as a
/// fast-path baseline for skipping future reads. An unpaired or
/// unreliable observation still carries a true hash of the bytes read.
struct DiskObservation {
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::uint64_t hash = 0;
    /// Filesystem identity of the inode the bytes were read from
    /// (fstat's UniqueID) — what binds a spelling to an entity.
    std::uint64_t uid_device = 0;
    std::uint64_t uid_file = 0;
    bool paired = false;
    bool reliable = false;
};

/// A completed observed read: the observation plus the text it hashed.
struct ObservedFile {
    DiskObservation obs;
    std::unique_ptr<llvm::MemoryBuffer> content;
};

namespace vfs {

/// A source file's text as every part of clice sees it: its bytes without
/// a leading UTF-8 byte order mark — the text an editor shows and sends.
/// Clang skips the mark as well, but would count it in every offset.
inline llvm::StringRef without_bom(llvm::StringRef bytes) {
    llvm::StringRef text = bytes;
    text.consume_front("\xEF\xBB\xBF");
    return text;
}

/// How a read serves a file.
enum class Read : std::uint8_t {
    /// Its text (see without_bom), copied into memory: sources, commands
    /// and configuration. Never mapped — Windows refuses an editor's save
    /// over a mapped file.
    Text,
    /// Its bytes as they are, copied into memory.
    Bytes,
    /// Its bytes as they are, mapped when large: build artifacts clice
    /// reads but never edits.
    Mapped,
};

/// A file, read whole.
std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::error_code> read(llvm::StringRef path,
                                                                         Read as = Read::Text);

/// A file's text read and hashed under the pairing discipline: open a
/// handle, fstat it, read through it, fstat again. Equal fstats prove
/// the stat describes the bytes (an in-place write racing the read moves
/// the mtime between the two fstats; a rename-over does not affect the
/// open handle at all). A post-fstat mtime inside the guard window
/// (coarse-granularity filesystems) demotes the pair to unreliable: a
/// racing write can land within one mtime tick, so such a stat must not
/// suppress future reads. Safe to call from any thread.
std::expected<ObservedFile, std::error_code> read_observed(llvm::StringRef path);

/// The file system one compile sees: the disk, each file served the way
/// read() serves it — sources as text, `#embed` data, PCH and PCM files
/// as bytes. Clang sets its working directory, so every compile gets its
/// own; an overlay wraps a fresh one.
class View : public llvm::vfs::ProxyFileSystem {
public:
    View() : ProxyFileSystem(llvm::vfs::createPhysicalFileSystem()) {}

    /// The size of the text: clang checks what it reads against it, and a
    /// PCH records it for every input. Status carries no use, so a binary
    /// use of a file clang only stat'ed first gets the text: `#embed` after
    /// `__has_embed` probed the same file reads it without the mark.
    llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine& path) override;

    /// Named by the path the OS opened (its RealName), which names include
    /// results and drives `-Wnonportable-include-path`.
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
        openFileForRead(const llvm::Twine& path) override;

    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
        openFileForReadBinary(const llvm::Twine& path) override {
        return getUnderlyingFS().openFileForReadBinary(path);
    }
};

}  // namespace vfs

}  // namespace clice
