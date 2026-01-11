#pragma once
#include <expected>

#include "PluginProtocol.h"
#include "Async/Async.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

// clang-format off
/// Run `python scripts/plugin-def.py update` to update the hash.
#define CLICE_PLUGIN_DEF_HASH "sha256:c46f7edfda0455327c65d40b9315ad5dc39153326c8cc63f1d8de2e2d0e7735a"
// clang-format on

namespace clice {

/// The hash of the definitions exposed to server plugins.
constexpr std::string_view plugin_definition_hash = CLICE_PLUGIN_DEF_HASH;

class Server;

struct ServerPluginBuilder;

/// A loaded server plugin.
///
/// An instance of this class wraps a loaded server plugin and gives access to its interface.
class Plugin {
public:
    /// Attempts to load a server plugin from a given file.
    ///
    /// Returns an error if either the library cannot be found or loaded,
    /// there is no public entry point, or the plugin implements the wrong API
    /// version.
    static std::expected<Plugin, std::string> load(const std::string& file_path);

    /// Gets the file path of the loaded plugin.
    llvm::StringRef file_path() const;

    /// Gets the name of the loaded plugin.
    llvm::StringRef name() const;

    /// Gets the version of the loaded plugin.
    llvm::StringRef version() const;

    /// Registers the server callbacks for the loaded plugin.
    void register_server_callbacks(ServerPluginBuilder& builder) const;

public:
    struct Self;

    Plugin(Self* self) : self(self) {}

    Self* operator->() {
        return self;
    }

protected:
    Self* self;
};

}  // namespace clice
