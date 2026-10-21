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
        auto on = line.find(" on /");
        auto type = line.rfind(" type ");
        if(on == llvm::StringRef::npos || type == llvm::StringRef::npos || type < on) {
            continue;
        }
        table.mounts.emplace_back(line.slice(on + 4, type).str(), line.take_front(on).str());
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
        if(!rest.consume_front(point) ||
           (!rest.empty() && !rest.starts_with("/") && !llvm::StringRef(point).ends_with("/"))) {
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

std::optional<std::string> find_runtime(llvm::StringRef program) {
    auto holds_runtime = [](llvm::StringRef dir) {
        return vfs::exists(path::join(dir, "cygwin1.dll")) ||
               vfs::exists(path::join(dir, "msys-2.0.dll"));
    };
    if(auto beside = path::parent_path(program); !beside.empty() && holds_runtime(beside)) {
        return beside.str();
    }
    auto search = llvm::sys::Process::GetEnv("PATH");
    if(!search) {
        return std::nullopt;
    }
    llvm::SmallVector<llvm::StringRef> dirs;
    llvm::StringRef(*search).split(dirs, path::list_separator, -1, false);
    for(auto dir: dirs) {
        if(holds_runtime(dir)) {
            return dir.str();
        }
    }
    return std::nullopt;
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

std::optional<std::string> read_cygwin_symlink(llvm::StringRef path) {
    constexpr llvm::StringLiteral cookie = "!<symlink>";
    // A symlink file holds a path; the program a driver names is far larger.
    auto status = vfs::status(path);
    if(!status || !status->is_file() || status->stamp.size > 4096) {
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
