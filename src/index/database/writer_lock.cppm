module clice:index.database.writer_lock;

import :support.prelude;

namespace clice::index {

/// One writer per cache directory. The index's records and manifests
/// tolerate no second writer, and every build configuration's database
/// lives under the same cache directory, so the lock sits at its root:
/// the LSP server of the project holds it for its lifetime, else a
/// batch command (`clice index`, `clice lint --index`) for the run. An OS
/// advisory lock dies with its process, so a crash leaves nothing stale
/// behind. Held by the project that opened the store, not by the
/// database handle: a database condemned and reopened mid-session keeps
/// the directory.
class WriterLock {
public:
    /// Why acquire() took no lock.
    enum class Refused : std::uint8_t {
        /// Another process holds it (holder).
        Held,
        /// The lock file cannot be opened (logged).
        Unopenable,
    };

    static std::expected<WriterLock, Refused> acquire(llvm::StringRef cache_dir);

    /// Who holds the lock of `cache_dir`, for the user: `pid N` as the
    /// holder recorded it, empty when it recorded none.
    static std::string holder(llvm::StringRef cache_dir);

    WriterLock(WriterLock&& other) noexcept = default;

    WriterLock& operator=(WriterLock&& other) noexcept {
        release();
        file = std::move(other.file);
        return *this;
    }

    ~WriterLock() {
        release();
    }

private:
    explicit WriterLock(fs::File file) : file(std::move(file)) {}

    void release();

private:
    fs::File file;
};

/// What a serving writer records next to the lock, for the commands that
/// find the lock taken: where its control channel listens. Written only
/// by the process holding the lock, so a record found while the lock is
/// free is a crash's residue.
struct ServerEndpoint {
    std::uint32_t pid = 0;
    std::string version;

    /// The local socket, or the named pipe on Windows, it listens on.
    std::string socket;
};

/// False when the record could not be written (logged).
bool write_endpoint(llvm::StringRef cache_dir, const ServerEndpoint& endpoint);

void remove_endpoint(llvm::StringRef cache_dir);

/// A command's view of the writer before it decides how the index gets
/// updated: become the writer itself, ask the serving one, or give up.
struct WriterStatus {
    enum class State : std::uint8_t {
        /// Nobody holds the lock.
        Free,
        /// A clice server holds it and can be asked through `endpoint`.
        Server,
        /// Another process holds it and cannot be asked: a batch run, or
        /// a server whose record is missing or of another version.
        Held,
    };

    State state = State::Free;
    ServerEndpoint endpoint;

    /// Who holds the lock, for the user; empty when unknown.
    std::string holder;
};

/// Check the lock without keeping it: a momentary observation — the
/// caller's own acquisition is the real gate. Never from a process that
/// holds the lock: POSIX record locks are per process, and closing the
/// check's descriptor would release it.
WriterStatus check_writer(llvm::StringRef cache_dir);

/// What a command tells the user when the check found a writer it cannot
/// ask.
std::string held_writer_message(const WriterStatus& status, llvm::StringRef cache_dir);

}  // namespace clice::index
