#include "analysis/annotation.h"

#include <format>
#include <map>

#include "support/process.h"
#include "vfs/file_system.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

namespace clice::analysis {

const Annotation* Annotations::find(llvm::StringRef name) const {
    auto it =
        llvm::find_if(list, [&](const Annotation& annotation) { return annotation.name == name; });
    return it == list.end() ? nullptr : &*it;
}

double Annotations::value(llvm::StringRef name, llvm::StringRef path, double fallback) const {
    auto* annotation = find(name);
    if(!annotation) {
        return fallback;
    }
    auto it = annotation->values.find(path);
    return it == annotation->values.end() ? fallback : it->second;
}

std::expected<Annotation, std::string> read_annotation(llvm::StringRef path) {
    auto buffer = vfs::read(path);
    if(!buffer) {
        return std::unexpected(
            std::format("cannot read {}: {}", std::string_view(path), buffer.error().message()));
    }

    struct File {
        std::string name;
        std::string unit;
        std::map<std::string, double> values;
    };

    File file;
    if(auto result = kota::codec::json::from_string((*buffer)->getBuffer(), file); !result) {
        return std::unexpected(std::format("{} is not an annotation file: {}",
                                           std::string_view(path),
                                           result.error().message));
    }
    if(file.name.empty()) {
        return std::unexpected(std::format("{} names no annotation", std::string_view(path)));
    }
    Annotation annotation{.name = std::move(file.name), .unit = std::move(file.unit)};
    for(auto& [key, value]: file.values) {
        annotation.values[key] = value;
    }
    return annotation;
}

std::expected<Annotation, std::string> git_churn(llvm::StringRef workspace, llvm::StringRef since) {
    // --relative names the files from the workspace and keeps only
    // those under it, whatever the repository root is.
    auto [ended] = kota::run(execute({"git",
                                      "log",
                                      std::format("--since={}", std::string_view(since)),
                                      "--format=",
                                      "--name-only",
                                      "--relative",
                                      "-z"},
                                     /*capture_stdout=*/true,
                                     workspace.str()));
    auto& log = *ended;
    if(!log) {
        return std::unexpected(log.error());
    }

    Annotation annotation{.name = churn_annotation.str(), .unit = "commits"};
    // -z prints each path verbatim, where a newline-separated list would
    // quote the unusual ones.
    llvm::SmallVector<llvm::StringRef> paths;
    llvm::StringRef(*log).split(paths, '\0', -1, false);
    for(auto path: paths) {
        annotation.values[path] += 1;
    }
    return annotation;
}

}  // namespace clice::analysis
