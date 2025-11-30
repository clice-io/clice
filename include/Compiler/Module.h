#pragma once

#include <expected>
#include <string>
#include <vector>

#include "Support/Struct.h"

namespace clice {

class CompilationUnit;

struct CompilationParams;

struct ModuleInfo {
    /// Whether this module is an interface unit.
    /// i.e. has export module declaration.
    bool isInterfaceUnit = false;

    /// Module name.
    std::string name;

    /// Dependent modules of this module.
    std::vector<std::string> mods;
};

inherited_struct(PCMInfo, ModuleInfo) {
    /// PCM file path.
    std::string path;

    /// Source file path.
    std::string srcPath;

    /// Files involved in building this PCM(not include module).
    std::vector<std::string> deps;
};

/// If input file is module interface unit, return its module name.
/// Otherwise, return an empty string.
std::string scanModuleName(CompilationParams& params);

/// Run the preprocessor to scan the given module unit to
/// collect its module name and dependencies.
std::expected<ModuleInfo, std::string> scanModule(CompilationParams& params);

}  // namespace clice
