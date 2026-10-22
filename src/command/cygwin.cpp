module;

#include "modules/prelude.h"

module clice;

import :command.cygwin;
import :support.process;
import :vfs.file_system;
import :vfs.path;

namespace clice {

bool is_posix_path(llvm::StringRef path) {
    return path.starts_with("/") && !path::is_absolute(path);
}

MountTable MountTable::parse(llvm::StringRef listing) {
    MountTable table;
    llvm::SmallVector<llvm::StringRef> lines;
    listing.split(lines, '\n', -1, false);
    for(auto line: lines) {
        auto [windows, mount] = line.split(" on /");
        auto [point, type] = mount.rsplit(" type ");
        if(type.empty()) {
            continue;
        }
        table.mounts.emplace_back(("/" + point).str(), windows.str());
    }
    std::ranges::stable_sort(table.mounts, std::ranges::greater{}, [](const auto& mount) {
        return mount.first.size();
    });
    return table;
}

std::string MountTable::to_windows(llvm::StringRef posix) const {
    llvm::SmallString<256> folded(posix);
    llvm::sys::path::remove_dots(folded, /*remove_dot_dot=*/true, llvm::sys::path::Style::posix);
    for(auto& [point, directory]: mounts) {
        llvm::StringRef rest = folded;
        if(!rest.consume_front(point) || !(point == "/" || rest.empty() || rest.starts_with("/"))) {
            continue;
        }
        rest = rest.ltrim('/');
        std::string windows = directory;
        // A drive mounts as `C:`, which names its root only as `C:/`.
        if(!rest.empty() || windows.ends_with(":")) {
            if(!windows.ends_with("/")) {
                windows += '/';
            }
            windows += rest;
        }
        return windows;
    }
    // read_mount_table() takes only a table mounting the root.
    std::unreachable();
}

namespace {

/// The directories beside `program` and on PATH that hold a Cygwin or MSYS2
/// runtime, in the order Windows looks for a program's DLLs.
std::vector<std::string> runtime_dirs(llvm::StringRef program) {
    std::vector<std::string> dirs;
    auto add = [&](llvm::StringRef dir) {
        if(!dir.empty() && (vfs::exists(path::join(dir, "cygwin1.dll")) ||
                            vfs::exists(path::join(dir, "msys-2.0.dll")))) {
            dirs.push_back(dir.str());
        }
    };
    add(path::parent_path(program));
    if(auto search = llvm::sys::Process::GetEnv("PATH")) {
        llvm::SmallVector<llvm::StringRef> entries;
        llvm::StringRef(*search).split(entries, path::list_separator, -1, false);
        for(auto entry: entries) {
            add(entry);
        }
    }
    return dirs;
}

kota::task<std::expected<MountTable, std::string>> read_mount_table(std::string runtime) {
    auto mount = path::join(runtime, "mount.exe");
    auto output = co_await execute({mount}, "");
    if(!output) {
        co_return std::unexpected(std::move(output.error()));
    }
    if(!output->failure.empty()) {
        co_return std::unexpected(std::move(output->failure));
    }
    auto table = MountTable::parse(output->out);
    if(!std::ranges::contains(table.mounts, "/", &std::pair<std::string, std::string>::first)) {
        co_return std::unexpected(std::format("{} lists no root mount", mount));
    }
    co_return table;
}

}  // namespace

kota::task<std::expected<MountTable, std::string>>
    find_mount_table(std::string program, std::vector<std::string> written) {
    std::vector<std::string> misses;
    for(auto& dir: runtime_dirs(program)) {
        auto table = co_await read_mount_table(dir);
        if(!table) {
            misses.push_back(std::format("{}: {}", dir, table.error()));
            continue;
        }
        for(auto& file: written) {
            // A program is named as Cygwin names it, without `.exe`.
            auto windows = table->to_windows(file);
            if(vfs::exists(windows) || vfs::exists(windows + ".exe")) {
                co_return std::move(*table);
            }
        }
        misses.push_back(std::format("{} maps it to {}", dir, table->to_windows(written.front())));
    }
    if(misses.empty()) {
        co_return std::unexpected(
            program.empty()
                ? "PATH holds no Cygwin or MSYS2 runtime"
                : std::format("no Cygwin or MSYS2 runtime is beside {} or on PATH", program));
    }
    co_return std::unexpected(std::format("no runtime maps {} to a file that exists ({})",
                                          written.front(),
                                          llvm::join(misses, "; ")));
}

std::optional<std::string> read_cygwin_symlink(llvm::StringRef path) {
    constexpr llvm::StringLiteral cookie = "!<symlink>";
    // A symlink file holds a path; the program a driver names is far larger.
    auto status = vfs::status(path);
    if(!status || status->stamp.size > 4096) {
        return std::nullopt;
    }
    auto buffer = vfs::read(path, vfs::Read::Bytes);
    if(!buffer) {
        return std::nullopt;
    }
    llvm::StringRef target = (*buffer)->getBuffer();
    if(!target.consume_front(cookie)) {
        return std::nullopt;
    }
    std::string utf8;
    // The runtime's own `ln -s` writes the target in UTF-16 behind a BOM,
    // the installer in plain bytes.
    if(target.starts_with("\xFF\xFE")) {
        if(!llvm::convertUTF16ToUTF8String(llvm::ArrayRef(target.data(), target.size()), utf8)) {
            return std::nullopt;
        }
        target = utf8;
    }
    return target.rtrim('\0').str();
}

}  // namespace clice
