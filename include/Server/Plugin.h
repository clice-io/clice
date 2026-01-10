#pragma once
#include <expected>

#include "Async/Async.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

// clang-format off
/// Run `python scripts/plugin-def.py update` to update the hash.
#define CLICE_PLUGIN_DEF_HASH "sha256:e35ff1adfbc385f7bae605e82ca4e7e70cfbf0f933bb8d57169d0d29abe5b6f7"
// clang-format on

namespace clice {

class Server;

#define CLICE_PLUGIN_PROTOCOL

#include "PluginDef.h"
#undef CLICE_PLUGIN_PROTOCOL

struct ServerPluginBuilder;

/// A loaded server plugin.
///
/// An instance of this class wraps a loaded server plugin and gives access to its interface.
class Plugin {
public:
    struct Self;

    Plugin(Self* self) : self(self) {}

    Self* operator->() {
        return self;
    }

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

protected:
    Self* self;
};

struct ServerPluginBuilder {
public:
    ServerPluginBuilder(ServerRef server_ref) : server_ref(server_ref) {}

#define CliceServerPluginAPI(METHOD, ...) void METHOD(void* plugin_data, __VA_ARGS__)

#include "PluginDef.h"
#undef CliceServerPluginAPI

protected:
    ServerRef server_ref;
};

}  // namespace clice
