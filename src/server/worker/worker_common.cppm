/// Shared utilities for stateful and stateless worker processes.

export module clice.worker:worker_common;

import stdlib;
import clice.compile;
import clice.protocol;

export namespace clice {

/// Fill CompilationParams directory and arguments from worker request fields.
inline void fill_args(CompilationParams& cp,
                      const std::string& directory,
                      const std::vector<std::string>& arguments) {
    cp.directory = directory;
    for(auto& arg: arguments) {
        cp.arguments.push_back(arg.c_str());
    }
}

}  // namespace clice
