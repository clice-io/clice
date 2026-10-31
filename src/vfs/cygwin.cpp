module clice;

import :support.process;
import :vfs.cygwin;
import :vfs.path;

namespace clice {

static async::task<std::expected<fs::cygwin::MountTable, std::string>>
    read_mount_table(std::string runtime) {
    auto mount = path::join(runtime, "mount.exe");
    auto output = co_await execute({mount}, "");
    if(!output) {
        co_return std::unexpected(std::move(output.error()));
    }
    if(!output->failure.empty()) {
        co_return std::unexpected(std::move(output->failure));
    }
    auto table = fs::cygwin::MountTable::parse(output->out);
    if(!table) {
        co_return std::unexpected(std::format("{} lists no root mount", mount));
    }
    co_return std::move(*table);
}

async::task<std::expected<fs::cygwin::MountTable, std::string>>
    find_mount_table(std::string program, std::vector<std::string> written) {
    std::string search;
    sys::get_env(search, "PATH");
    std::vector<std::string> search_path;
    for(auto dir: sys::split_paths(search)) {
        search_path.emplace_back(dir);
    }
    std::vector<std::string> misses;
    for(auto& dir: fs::cygwin::runtime_dirs(program, search_path)) {
        auto table = co_await read_mount_table(dir);
        if(!table) {
            misses.push_back(std::format("{}: {}", dir, table.error()));
            continue;
        }
        std::string windows;
        auto missing = ranges::find_if(written, [&](const std::string& file) {
            table->to_windows(windows, file);
            // A program is named as Cygwin names it, without `.exe`.
            return !fs::exists(windows) && !fs::exists(windows + ".exe");
        });
        if(missing == written.end()) {
            co_return std::move(*table);
        }
        misses.push_back(std::format("{} maps {} to {}", dir, *missing, windows));
    }
    if(misses.empty()) {
        co_return std::unexpected(
            program.empty()
                ? "PATH holds no Cygwin or MSYS2 runtime"
                : std::format("no Cygwin or MSYS2 runtime is beside {} or on PATH", program));
    }
    co_return std::unexpected(std::format("no runtime maps {} to files that exist ({})",
                                          llvm::join(written, " and "),
                                          llvm::join(misses, "; ")));
}

}  // namespace clice
