module;

#include "support/prelude.h"

#include "support/logging.macros.h"

module clice;

import :support.logging;
import :vfs.file_system;
import :vfs.path;

namespace clice::vfs {

namespace {

/// The text of an open file (see without_bom).
llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> load_text(const fs::File& file,
                                                             const llvm::Twine& name) {
    auto buffer = llvm::MemoryBuffer::getOpenFile(file.native_handle(),
                                                  name,
                                                  /*FileSize=*/-1,
                                                  /*RequiresNullTerminator=*/true,
                                                  /*IsVolatile=*/true);
    if(!buffer) {
        return buffer;
    }
    auto text = without_bom((*buffer)->getBuffer());
    if(text.size() == (*buffer)->getBufferSize()) {
        return buffer;
    }
    return llvm::MemoryBuffer::getMemBufferCopy(text, (*buffer)->getBufferIdentifier());
}

bool read_starts_with_bom(llvm::StringRef path) {
    auto file = fs::File::open(path);
    if(!file) {
        return false;
    }
    char head[3];
    auto read = file->read(head);
    return read && *read == 3 && without_bom(llvm::StringRef(head, 3)).empty();
}

/// Whether the file a stamp describes starts with the mark, peeked once
/// per stamp: adding or removing the mark changes the size, so a result
/// holds as long as clang's own size-and-mtime check of what it read.
bool starts_with_bom(const Stamp& stamp, llvm::StringRef path) {
    using Key = std::tuple<std::uint64_t,
                           std::uint64_t,
                           std::uint64_t,
                           std::uint64_t,
                           std::int64_t,
                           std::int64_t>;
    static std::mutex mutex;
    static llvm::DenseMap<Key, bool> results;
    Key key{stamp.id.device,
            stamp.id.file,
            stamp.id.file_high,
            stamp.size,
            stamp.mtime_ns,
            stamp.ctime_ns};
    {
        std::lock_guard lock(mutex);
        if(auto it = results.find(key); it != results.end()) {
            return it->second;
        }
    }
    auto has_bom = read_starts_with_bom(path);
    std::lock_guard lock(mutex);
    results.try_emplace(key, has_bom);
    return has_bom;
}

#ifdef _WIN32

/// A directory asked about this often within one operation is listed.
constexpr unsigned list_after = 8;

/// A directory with more entries is not kept: the few names asked about
/// it would not pay for all of them.
constexpr std::size_t list_limit = 4096;

/// The statuses of a directory's entries, links left out; empty when it
/// is remote, cannot be listed or holds too many.
llvm::StringMap<fs::Status> list_statuses(llvm::StringRef dir) {
    llvm::StringMap<fs::Status> entries;
    if(auto volume = fs::volume(dir); !volume || volume->remote) {
        return entries;
    }
    auto listed = fs::read_dir_status(dir);
    if(!listed || listed->size() > list_limit) {
        return entries;
    }
    for(auto& entry: *listed) {
        if(entry.status.type != fs::FileType::Symlink) {
            entries.try_emplace(entry.name, entry.status);
        }
    }
    return entries;
}

#endif

/// Serves a buffer the process keeps to one compile.
class SharedBuffer : public llvm::MemoryBuffer {
public:
    SharedBuffer(std::shared_ptr<const llvm::MemoryBuffer> owner, bool requires_terminator) :
        owner(std::move(owner)) {
        init(this->owner->getBufferStart(), this->owner->getBufferEnd(), requires_terminator);
    }

    llvm::StringRef getBufferIdentifier() const override {
        return owner->getBufferIdentifier();
    }

    BufferKind getBufferKind() const override {
        return owner->getBufferKind();
    }

private:
    std::shared_ptr<const llvm::MemoryBuffer> owner;
};

/// Source texts kept across the compiles of a process, by the path they
/// were opened under: every compile reads each header it includes, and
/// most are the bytes the compile before read. A text is served again
/// while a status of its file matches the one it was read under — size,
/// mtime and ID, the evidence the master's own change checks rest on — and
/// only a text read in one piece whose mtime lay outside the guard window
/// is kept at all.
class TextCache {
public:
    struct Entry {
        fs::Status status;
        std::string real_name;
        std::shared_ptr<const llvm::MemoryBuffer> text;
        std::uint64_t used = 0;
    };

    std::optional<Entry> find(llvm::StringRef path, const fs::Status& status) {
        std::lock_guard lock(mutex);
        auto it = entries.find(path);
        if(it == entries.end() || Stamp::of(it->second.status) != Stamp::of(status)) {
            return std::nullopt;
        }
        clock += 1;
        it->second.used = clock;
        return it->second;
    }

    void insert(llvm::StringRef path, Entry entry) {
        if(entry.text->getBufferSize() > budget) {
            return;
        }
        std::lock_guard lock(mutex);
        clock += 1;
        entry.used = clock;
        auto [it, inserted] = entries.try_emplace(path);
        if(!inserted) {
            bytes -= cost(*it);
        }
        it->second = std::move(entry);
        bytes += cost(*it);
        while(bytes > budget) {
            auto oldest = ranges::min_element(entries, {}, [](const auto& candidate) {
                return candidate.second.used;
            });
            bytes -= cost(*oldest);
            entries.erase(oldest);
        }
    }

private:
    constexpr static std::size_t budget = 128 << 20;

    /// An entry's memory, its bookkeeping included: a cache of empty
    /// headers is not free.
    static std::size_t cost(const llvm::StringMapEntry<Entry>& entry) {
        return entry.getKeyLength() + entry.second.real_name.size() +
               entry.second.text->getBufferSize() + 256;
    }

    std::mutex mutex;
    llvm::StringMap<Entry> entries;
    std::size_t bytes = 0;
    std::uint64_t clock = 0;
};

TextCache& texts() {
    static TextCache cache;
    return cache;
}

/// Mappings of the artifacts keep_mapped() named, most recently named
/// first. A mapping is used while the file stats as it did when mapped.
class MappedArtifacts {
public:
    void keep(llvm::StringRef path) {
        auto key = spelling(path);
        std::lock_guard lock(mutex);
        auto it = ranges::find(entries, key, &Entry::path);
        if(it == entries.end()) {
            entries.insert(entries.begin(), Entry{.path = std::move(key)});
            if(entries.size() > capacity) {
                entries.pop_back();
            }
        } else {
            std::rotate(entries.begin(), it, it + 1);
        }
    }

    /// The mapping of `file`, open at `path`, kept when `path` was named;
    /// nullptr when it was not.
    std::shared_ptr<const llvm::MemoryBuffer> map(llvm::StringRef path,
                                                  const fs::File& file,
                                                  const fs::Status& status) {
        auto key = spelling(path);
        std::lock_guard lock(mutex);
        auto it = ranges::find(entries, key, &Entry::path);
        if(it == entries.end()) {
            return nullptr;
        }
        if(!it->buffer || Stamp::of(it->status) != Stamp::of(status)) {
            auto mapped = llvm::MemoryBuffer::getOpenFile(file.native_handle(),
                                                          path,
                                                          status.size,
                                                          /*RequiresNullTerminator=*/false,
                                                          /*IsVolatile=*/false);
            if(!mapped) {
                return nullptr;
            }
            it->status = status;
            it->buffer = std::move(*mapped);
        }
        return it->buffer;
    }

private:
    /// The master names a PCH as the store spells it, clang as the
    /// compile's arguments do (on Windows the two mix separators
    /// differently): compare them in one spelling.
    static std::string spelling(llvm::StringRef path) {
        std::string key = path.str();
        path::canonicalize(key);
        return key;
    }

    struct Entry {
        std::string path;
        fs::Status status;
        std::shared_ptr<const llvm::MemoryBuffer> buffer;
    };

    /// PCHs of the documents a worker serves in turn.
    constexpr static std::size_t capacity = 8;

    std::mutex mutex;
    std::vector<Entry> entries;
};

MappedArtifacts& mapped() {
    static MappedArtifacts artifacts;
    return artifacts;
}

/// A source file served from the process's texts.
class CachedText : public llvm::vfs::File {
public:
    CachedText(std::string name, TextCache::Entry entry) :
        name(std::move(name)), entry(std::move(entry)) {}

    llvm::ErrorOr<llvm::vfs::Status> status() override {
        return llvm::vfs::Status::copyWithNewSize(to_llvm(entry.status, name),
                                                  entry.text->getBufferSize());
    }

    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
        getBuffer(const llvm::Twine&, int64_t, bool, bool) override {
        return std::make_unique<SharedBuffer>(entry.text, true);
    }

    llvm::ErrorOr<std::string> getName() override {
        return entry.real_name.empty() ? name : entry.real_name;
    }

    std::error_code close() override {
        return {};
    }

private:
    std::string name;
    TextCache::Entry entry;
};

/// A file a compile opened, answering its status through the handle it
/// was opened by. A source file is served as its text, read once, the size
/// its status reports agreeing with the text; a binary file as its bytes,
/// read the way clang asks.
class DiskFile : public llvm::vfs::File {
public:
    /// `key` is the absolute path the process keeps the file's text or
    /// mapping under.
    DiskFile(fs::File file, std::string name, std::string real_name, std::string key, bool text) :
        file(std::move(file)), name(std::move(name)), real_name(std::move(real_name)),
        key(std::move(key)), is_text(text) {}

    llvm::ErrorOr<llvm::vfs::Status> status() override {
        auto status = file.status();
        if(!status) {
            return status.error();
        }
        auto result = to_llvm(*status, name);
        if(!is_text || status->type != fs::FileType::Regular) {
            return result;
        }
        if(auto error = load()) {
            return error;
        }
        return llvm::vfs::Status::copyWithNewSize(result, text->getBufferSize());
    }

    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> getBuffer(const llvm::Twine& buffer_name,
                                                                 int64_t size,
                                                                 bool requires_terminator,
                                                                 bool is_volatile) override {
        if(!is_text) {
            if(!requires_terminator && !is_volatile) {
                if(auto status = file.status()) {
                    if(auto kept = mapped().map(key, file, *status)) {
                        return std::make_unique<SharedBuffer>(std::move(kept), false);
                    }
                }
            }
            return llvm::MemoryBuffer::getOpenFile(file.native_handle(),
                                                   buffer_name,
                                                   size,
                                                   requires_terminator,
                                                   is_volatile);
        }
        if(auto error = load()) {
            return error;
        }
        return std::make_unique<SharedBuffer>(text, true);
    }

    llvm::ErrorOr<std::string> getName() override {
        return real_name.empty() ? name : real_name;
    }

    std::error_code close() override {
        return file.close();
    }

private:
    std::error_code load() {
        if(text) {
            return {};
        }
        auto before = file.status();
        auto loaded = load_text(file, name);
        if(!loaded) {
            return loaded.getError();
        }
        text = std::move(*loaded);
        auto after = file.status();
        if(before && after && after->type == fs::FileType::Regular &&
           Stamp::of(*before) == Stamp::of(*after) && stable_mtime(Stamp::of(*after).mtime_ns)) {
            texts().insert(key, {.status = *after, .real_name = real_name, .text = text});
        }
        return {};
    }

    fs::File file;
    std::string name;
    std::string real_name;
    std::string key;
    bool is_text;
    std::shared_ptr<const llvm::MemoryBuffer> text;
};

}  // namespace

std::expected<std::string, std::error_code> read_text(llvm::StringRef path) {
    std::string text;
    if(auto error = fs::read_file(text, path)) {
        return std::unexpected(error);
    }
    text.erase(0, text.size() - without_bom(text).size());
    return text;
}

std::expected<ObservedFile, std::error_code> read_observed(llvm::StringRef path) {
    auto file = fs::File::open(path);
    if(!file) {
        return std::unexpected(file.error());
    }

    auto before = file->status();

    // The bytes must be a snapshot taken between the two fstats — a mapped
    // buffer would keep tracking the file after the post-fstat, unpairing
    // hash and stat.
    auto buffer = load_text(*file, path);
    if(!buffer) {
        return std::unexpected(buffer.getError());
    }

    ObservedFile result;
    result.content = std::move(*buffer);
    result.obs.hash = llvm::xxh3_64bits(result.content->getBuffer());

    auto after = file->status();
    if(!after) {
        return result;
    }
    result.obs.stamp = Stamp::of(*after);
    result.obs.paired = before && Stamp::of(*before) == result.obs.stamp;
    result.obs.reliable = result.obs.paired && stable_mtime(result.obs.stamp.mtime_ns);
    return result;
}

llvm::vfs::Status to_llvm(const fs::Status& status, llvm::StringRef name) {
    using llvm::sys::fs::file_type;
    auto type = [&] {
        switch(status.type) {
            case fs::FileType::Regular: return file_type::regular_file;
            case fs::FileType::Directory: return file_type::directory_file;
            case fs::FileType::Symlink: return file_type::symlink_file;
            case fs::FileType::Character: return file_type::character_file;
            case fs::FileType::Block: return file_type::block_file;
            case fs::FileType::Fifo: return file_type::fifo_file;
            case fs::FileType::Socket: return file_type::socket_file;
            case fs::FileType::Unknown: return file_type::type_unknown;
        }
        std::unreachable();
    }();
    // ReFS file IDs take all 128 bits.
    auto file =
        status.id.file_high == 0
            ? status.id.file
            : static_cast<std::uint64_t>(llvm::hash_combine(status.id.file, status.id.file_high));
    return llvm::vfs::Status(name,
                             llvm::sys::fs::UniqueID(status.id.device, file),
                             llvm::sys::TimePoint<>(status.mtime.time_since_epoch()),
                             0,
                             0,
                             status.size,
                             type,
                             llvm::sys::fs::all_all);
}

std::expected<fs::Status, std::error_code> StatusBatch::status(llvm::StringRef path) {
#ifdef _WIN32
    auto dir = path::parent(path);
    auto& directory = directories[dir];
    if(!directory.listed) {
        directory.asked += 1;
        if(directory.asked < list_after) {
            auto status = fs::status(path);
            // NTFS updates a directory entry's size and time only for the
            // link a write went through: a directory of hard links (Boost's
            // `b2 headers`) cannot be answered from its listing.
            if(status && status->links > 1) {
                directory.listed = true;
            }
            return status;
        }
        directory.listed = true;
        directory.entries = list_statuses(dir);
    }
    if(auto it = directory.entries.find(path::filename(path)); it != directory.entries.end()) {
        return it->second;
    }
#endif
    return fs::status(path);
}

bool HiddenFiles::hides(llvm::StringRef path) const {
    if(dirs.empty()) {
        return false;
    }
    auto spelled = Spelling::absolute(path);
    for(auto& dir: dirs) {
        llvm::StringRef name = spelled;
        if(name.consume_front(dir.path) && name.consume_front("/") &&
           ranges::binary_search(dir.names, name, {}, [](llvm::StringRef hidden) {
               return hidden;
           })) {
            return true;
        }
    }
    return false;
}

llvm::ErrorOr<llvm::vfs::Status> View::status(const llvm::Twine& path) {
    llvm::SmallString<256> absolute;
    path.toVector(absolute);
    if(auto error = getUnderlyingFS().makeAbsolute(absolute)) {
        return error;
    }
    if(hidden.hides(absolute)) {
        return std::make_error_code(std::errc::no_such_file_or_directory);
    }
    auto status = statuses.status(absolute);
    if(!status) {
        return status.error();
    }
    auto result = to_llvm(*status, path.str());
    if(status->type != fs::FileType::Regular || status->size < 3 ||
       !starts_with_bom(Stamp::of(*status), absolute)) {
        return result;
    }
    return llvm::vfs::Status::copyWithNewSize(result, status->size - 3);
}

llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> View::openFileForRead(const llvm::Twine& path) {
    return open(path, true);
}

llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
    View::openFileForReadBinary(const llvm::Twine& path) {
    return open(path, false);
}

llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> View::open(const llvm::Twine& path, bool text) {
    llvm::SmallString<256> absolute;
    path.toVector(absolute);
    if(auto error = getUnderlyingFS().makeAbsolute(absolute)) {
        return error;
    }
    if(hidden.hides(absolute)) {
        return std::make_error_code(std::errc::no_such_file_or_directory);
    }
    if(text) {
        auto status = statuses.status(absolute);
        if(!status) {
            return status.error();
        }
        if(auto kept = texts().find(absolute, *status)) {
            return std::make_unique<CachedText>(path.str(), std::move(*kept));
        }
    }
    auto file = fs::File::open(absolute.str());
    if(!file) {
        return file.error();
    }
    std::string real_name;
    if(fs::canonical(real_name, absolute.str())) {
        real_name.clear();
    }
    return std::make_unique<DiskFile>(std::move(*file),
                                      path.str(),
                                      std::move(real_name),
                                      absolute.str().str(),
                                      text);
}

void keep_mapped(llvm::StringRef path) {
    mapped().keep(path);
}

bool stable_mtime(std::int64_t mtime_ns) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    return mtime_ns <= stat_baseline_before_ns(now_ms);
}

void walk(llvm::StringRef root,
          llvm::function_ref<bool(llvm::StringRef path, fs::FileType type)> visit) {
    auto entries = fs::read_dir(root);
    if(!entries) {
        if(entries.error() != std::errc::no_such_file_or_directory) {
            LOG_WARN("Cannot read directory {}: {}", root, entries.error().message());
        }
        return;
    }
    for(auto& entry: *entries) {
        auto path = path::join(root, entry.name);
        if(visit(path, entry.type) && entry.type == fs::FileType::Directory) {
            walk(path, visit);
        }
    }
}

}  // namespace clice::vfs
