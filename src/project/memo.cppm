module clice:project.memo;

import :support.prelude;
import :vfs.file_table;

namespace clice {

/// The results of one kind of work, remembered by request with the trace of
/// what each read: the files, at the versions read. lookup() is the one
/// check of whether a result still holds; one whose inputs changed is
/// forgotten there. What a result is computed again from decides the trace:
/// the files a build read, or for a crash, which reports nothing it read,
/// the inputs known before it ran.
template <typename Key, typename Result>
class Memo {
public:
    /// How a key is passed: a string one as a StringRef.
    using KeyRef = std::conditional_t<std::same_as<Key, std::string>, llvm::StringRef, const Key&>;

    struct Entry {
        Result result;

        DepsSnapshot trace;
    };

    /// The entry of `key` while the inputs its trace names hold.
    Entry* lookup(FileTable& files, KeyRef key) {
        auto it = entries.find(key);
        if(it == entries.end()) {
            return nullptr;
        }
        if(deps_changed(files, it->second.trace)) {
            entries.erase(it);
            return nullptr;
        }
        return &it->second;
    }

    /// The entry of `key`, its inputs checked or not.
    Entry* find(KeyRef key) {
        auto it = entries.find(key);
        return it != entries.end() ? &it->second : nullptr;
    }

    const Entry* find(KeyRef key) const {
        auto it = entries.find(key);
        return it != entries.end() ? &it->second : nullptr;
    }

    /// The entry of `key`, its inputs checked or not, made with an empty
    /// result and trace when there is none.
    Entry& entry(KeyRef key) {
        return entries[key];
    }

    Entry& commit(KeyRef key, Result result, DepsSnapshot trace) {
        auto& entry = entries[key];
        entry = {.result = std::move(result), .trace = std::move(trace)};
        return entry;
    }

    /// Returns whether there was an entry.
    bool forget(KeyRef key) {
        return entries.erase(key);
    }

    /// Visit every entry with its key, checked or not.
    template <typename Visit>
    void for_each(Visit&& visit) const {
        for(auto& [key, entry]: entries) {
            visit(key, entry);
        }
    }

private:
    std::conditional_t<std::same_as<Key, std::string>,
                       llvm::StringMap<Entry>,
                       llvm::DenseMap<Key, Entry>>
        entries;
};

}  // namespace clice
