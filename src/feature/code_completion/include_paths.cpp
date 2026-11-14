module clice;

import :command.search_config;
import :command.table;
import :feature.code_completion.cursor;
import :feature.code_completion;
import :support.fuzzy_matcher;
import :syntax.position;
import :vfs.dir_cache;
import :vfs.path;

namespace clice::feature {

/// Whether a file looks like a header, by clang's own include-completion
/// filter widened by the fragment extensions projects include (`.inl`,
/// `.ipp`, `.def`, ...): a header extension anywhere, no extension (the
/// standard library's, Qt's, frameworks') only where such headers live.
static bool looks_like_header(llvm::StringRef name,
                              llvm::StringRef dir,
                              llvm::StringRef search_dir,
                              bool system) {
    auto extension = llvm::StringRef(path::extension(name)).lower();
    if(ranges::contains(std::array{".h",
                                   ".hh",
                                   ".hpp",
                                   ".hxx",
                                   ".h++",
                                   ".inc",
                                   ".inl",
                                   ".ipp",
                                   ".tcc",
                                   ".tpp",
                                   ".txx",
                                   ".def",
                                   ".cuh"},
                        extension)) {
        return true;
    }
    if(name.contains('.')) {
        return false;
    }
    auto dir_name = path::filename(dir);
    return system || dir_name.starts_with("Qt") || dir_name == "ActiveQt" ||
           dir.ends_with(".framework/Headers") || path::filename(search_dir) == "include";
}

auto include_completion(const CompletionName& completing,
                        llvm::StringRef text,
                        const PositionMap& map,
                        PositionEncoding encoding,
                        llvm::StringRef path,
                        const SearchConfig& search,
                        vfs::Scope& scope,
                        std::uint32_t limit) -> protocol::CompletionList {
    bool angled = (completing.kind == CompletionName::Kind::IncludeAngled);
    llvm::StringRef dir_prefix;
    llvm::StringRef file_prefix = completing.prefix;
    if(auto slash = file_prefix.rfind('/'); slash != llvm::StringRef::npos) {
        dir_prefix = file_prefix.take_front(slash);
        file_prefix = file_prefix.drop_front(slash + 1);
    }

    // A directory leaves the path open: the `/` typed after it asks
    // again, for the directory's own entries. A header ends the path
    // and closes the directive, replacing the rest of the path through
    // a closing delimiter already there.
    char closer = angled ? '>' : '"';
    auto item_of = [&](llvm::StringRef name, bool is_dir, bool header, std::uint64_t rank) {
        auto end = completing.replace.end;
        if(!is_dir && completing.closed) {
            end = static_cast<std::uint32_t>(text.find(closer, end)) + 1;
        }
        protocol::CompletionItem item;
        item.label = is_dir ? (name + "/").str() : name.str();
        item.kind =
            is_dir ? protocol::CompletionItemKind::Folder : protocol::CompletionItemKind::File;
        item.sort_text = std::format("{:010}", rank);
        // An editor draws a file's icon by its name's extension, which a
        // header like `vector` lacks.
        if(header) {
            item.data = protocol::LSPAny(std::string("header"));
        }
        item.text_edit = protocol::TextEdit{
            .range = *map.range({completing.replace.begin, end}, encoding),
            .new_text = is_dir ? name.str() : (name + llvm::Twine(closer)).str(),
        };
        return item;
    };

    FuzzyMatcher matcher(file_prefix);
    auto builtin_headers = Spelling::absolute(path::join(resource_dir(), "include")).str();
    std::vector<protocol::CompletionItem> items;
    // A name several directories hold is listed once, at its best rank: a
    // file no header would be named as does not hide a header behind it.
    // Each name's item and rank.
    llvm::StringMap<std::pair<std::size_t, std::uint64_t>> listed;
    // clang takes the first framework directory holding the framework: a
    // header only a later one's has does not resolve.
    bool framework_found = false;

    auto collect = [&](const SearchDir& search_dir, std::uint64_t tier, std::uint64_t position) {
        llvm::SmallString<256> dir(search_dir.path);
        bool frameworks = search_dir.framework && dir_prefix.empty();
        if(search_dir.framework && !dir_prefix.empty()) {
            auto [name, rest] = dir_prefix.split('/');
            auto bundle = (name + ".framework").str();
            if(framework_found || !scope.list(search_dir.path).contains(bundle)) {
                return;
            }
            framework_found = true;
            path::push(dir, bundle);
            path::push(dir, "Headers");
            if(!rest.empty()) {
                path::push(dir, rest);
            }
        } else if(!dir_prefix.empty()) {
            path::push(dir, dir_prefix);
        }
        for(auto& entry: scope.list(dir).entries) {
            llvm::StringRef name = entry.getKey();
            bool is_dir = entry.getValue();
            if(frameworks && !(is_dir && name.consume_back(".framework"))) {
                continue;
            }
            auto score = matcher.match(name);
            if(!score) {
                continue;
            }

            auto internal = [&] {
                auto below = dir_prefix.empty() ? name.str() : (dir_prefix + "/" + name).str();
                return search_dir.path == builtin_headers &&
                       builtin_header(below) == BuiltinHeader::Internal;
            };
            bool hidden = name.starts_with(".");
            bool header = !is_dir && looks_like_header(name, dir, search_dir.path, tier >= 3);
            bool unlikely = !is_dir && (!header || internal());
            bool reserved = name.starts_with("_");
            // The rank, ascending. A hidden name (`.git/`) follows all the
            // others, and a file no header would be named as (`README.md`,
            // `main.cpp`), or a builtin header that must not be included
            // directly, the rest. Then the better the typed name matches: the
            // whole name, a prefix, words of it (`v` finds `string_view`); the
            // score is in [0, 2]. Then where the search found it (`tier`): the
            // includer's directory, the quoted then the angled search
            // directories, the system ones the command names, last the
            // toolchain's own; in search order within each, after the others
            // of its group when the name is reserved (`_`-led: a library's
            // internals, libc++'s `__algorithm/`, MinGW's `_mingw.h`).
            auto mismatch = static_cast<std::uint64_t>((2.0f - *score) * 1000.0f);
            auto rank = (std::uint64_t(hidden) << 32) | (std::uint64_t(unlikely) << 31) |
                        (mismatch << 20) | (tier << 17) | (std::uint64_t(reserved) << 16) |
                        position;
            auto [it, inserted] = listed.try_emplace(name, items.size(), rank);
            if(inserted) {
                items.push_back(item_of(name, is_dir, header, rank));
            } else if(rank < it->second.second) {
                it->second.second = rank;
                items[it->second.first] = item_of(name, is_dir, header, rank);
            }
        }
    };

    // A quoted include looks next to the file that contains it first.
    if(!angled) {
        collect({.path = std::string(path::parent(path))}, 0, 0);
    }
    for(unsigned i = angled ? search.angled_start_idx : 0; i < search.dirs.size(); i += 1) {
        std::uint64_t tier = i < search.angled_start_idx   ? 1
                             : i < search.system_start_idx ? 2
                             : search.dirs[i].driver       ? 4
                                                           : 3;
        collect(search.dirs[i], tier, i + 1);
    }
    return completion_list(std::move(items), limit);
}

}  // namespace clice::feature
