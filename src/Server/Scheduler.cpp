#include "Server/Config.h"
#include "Server/Scheduler.h"
#include "Server/Server.h"

namespace clice {

async::promise<void> Scheduler::updatePCH(llvm::StringRef filepath,
                                          llvm::StringRef content,
                                          llvm::StringRef command) {
    auto [iter, success] = pchs.try_emplace(filepath);
    if(success || iter->second.needUpdate(content)) {
        Tracer tracer;

        CompliationParams params;
        params.content = content;
        params.srcPath = filepath;
        params.outPath = filepath;
        params.command = command;
        params.computeBounds();

        path::replace_path_prefix(params.outPath,
                                  config::workplace(),
                                  config::frontend().cache_directory);
        path::replace_extension(params.outPath, ".pch");

        log::info("Start building PCH for {0} at {1}", params.srcPath, params.outPath);

        PCHInfo pch;

        co_await async::schedule_task([&] {
            auto dir = path::parent_path(params.outPath);
            if(!fs::exists(dir)) {
                if(auto error = fs::create_directories(dir)) {
                    log::fatal("Failed to create directory {0}, because {1}, build PCH stopped",
                               dir,
                               error.message());
                    return;
                }
            }

            if(auto info = compile(params, pch); !info) {
                log::fatal("Failed to build PCH for {0}, because {1}",
                           filepath.str(),
                           info.takeError());
                return;
            }
        });

        log::info("PCH for {0} is up-to-date, elapsed {1}ms",
                  filepath.str(),
                  tracer.duration().count());

        pchs[filepath] = std::move(pch);
    } else {
        log::info("Reuse PCH for {0} from {1}", filepath.str(), iter->second.path);
    }
    co_return;
}

async::promise<void> Scheduler::buildAST(llvm::StringRef filepath, llvm::StringRef content) {
    llvm::SmallString<128> path = filepath;

    auto [iter, success] = files.try_emplace(filepath);
    if(!success && !iter->second.isIdle) {
        /// If the file is already existed and is building, append the task to the waiting list.
        co_await async::suspend([&](auto handle) {
            iter->second.waitings.emplace_back(Task{
                .isBuild = true,
                .waiting = handle,
            });
        });
    }

    files[filepath].isIdle = false;

    static bool initCmd = false;
    if(!initCmd) {
        cmdMgr.update(config::frontend().compile_commands_directorys[0]);
        initCmd = true;
    }

    CompliationParams params;
    params.srcPath = path;
    params.content = content;
    params.command = cmdMgr.lookupFirst(filepath);

    /// through arguments to judge is it a module.
    bool isModule = false;
    co_await (isModule ? updatePCM() : updatePCH(filepath, content, params.command));

    params.addPCH(pchs.at(filepath));

    Tracer tracer;

    log::info("Start building AST for {0}, command: [{1}]", filepath, params.command.str());

    auto task = [&] {
        /// FIXME: We cannot use reference capture the `pch` here, beacuse the reference may be
        /// Invalid Because other changed the `pchs` map. We also cannot to retrieve the `pch` from
        /// the `pchs` map in this task, beacuse it is called in thread pool which will result in
        /// data race. So temporarily copy the `pch` here. There must be a better way to solve this
        /// problem.
        auto info = clice::compile(params);
        if(!info) {
            log::fatal("Failed to build AST for {0}", filepath);
        }
        return std::move(*info);
    };

    auto compiler = co_await async::schedule_task(std::move(task));

    auto& file = files[path];
    file.content = content;
    file.compiler = std::move(compiler);

    log::info("Build AST successfully for {0}, elapsed {1}", filepath, tracer.duration());

    if(!file.waitings.empty()) {
        auto task = std::move(file.waitings.front());
        async::schedule(task.waiting);
        file.waitings.pop_front();
    }

    file.isIdle = true;
}

async::promise<proto::CompletionResult> Scheduler::codeComplete(llvm::StringRef filepath,
                                                                unsigned int line,
                                                                unsigned int column) {
    auto iter = files.find(filepath);
    if(iter == files.end()) {
        log::fatal("File {0} is not building, skip code completion", filepath);
    }

    if(iter->second.isIdle) {
        /// If the file is already existed and is building, append the task to the waiting list.
        co_await async::suspend([&](auto handle) {
            iter->second.waitings.emplace_back(Task{
                .isBuild = true,
                .waiting = handle,
            });
        });
    }

    llvm::SmallString<128> path = filepath;
    /// FIXME: lookup from CDB file and adjust and remove unnecessary arguments.1

    CompliationParams params;
    params.content = iter->second.content;
    params.srcPath = path;
    params.command = cmdMgr.lookupFirst(filepath);

    /// through arguments to judge is it a module.
    bool isModule = false;
    co_await (isModule ? updatePCM() : updatePCH(params.srcPath, params.content, params.command));
    params.addPCH(pchs.at(filepath));

    Tracer tracer;
    log::info("Run code completion at {0}:{1}:{2}", filepath, line, column);

    auto task = [&] {
        return feature::codeCompletion(params, line, column, filepath, {});
    };

    auto result = co_await async::schedule_task(std::move(task));

    log::info("Code completion for {0} is done, elapsed {1}", filepath, tracer.duration());

    co_return result;
}

async::promise<void> Scheduler::add(llvm::StringRef path, llvm::StringRef content) {
    co_await buildAST(path, content);
    co_return;
}

async::promise<void> Scheduler::update(llvm::StringRef path, llvm::StringRef content) {
    co_await buildAST(path, content);
    co_return;
}

async::promise<void> Scheduler::save(llvm::StringRef path) {
    co_return;
}

async::promise<void> Scheduler::close(llvm::StringRef path) {
    co_return;
}

}  // namespace clice
