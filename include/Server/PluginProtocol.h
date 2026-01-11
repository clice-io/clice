/// The API version of the clice plugin.
/// Update this version when you change:
/// - The definition of struct `PluginInfo`.
/// - The definition of function `clice_get_server_plugin_info`.
/// Note: you don't have to update this version if you only change other APIs, which is guaranteed
/// by the `PluginInfo::definition_hash`.
#ifndef CLICE_PLUGIN_API_VERSION
#define CLICE_PLUGIN_API_VERSION 1
#elif CLICE_PLUGIN_API_VERSION != 1
#error "CLICE_PLUGIN_API_VERSION must be 1, but got " CLICE_PLUGIN_API_VERSION
#endif

#include <cstdint>

#include "Async/Async.h"

#include "llvm/Support/Compiler.h"

namespace clice {

class Server;

struct ServerPluginBuilder;
/// Defines the library APIs that loads a plugin.
extern "C" {
    /// A C-compatible struct that contains information about the plugin.
    struct PluginInfo {
        /// The clice API version of the plugin.
        uint32_t api_version;
        /// The name of the plugin.
        const char* name;
        /// The version of the plugin.
        const char* version;
        /// The plugin definition hash.
        const char* definition_hash;
        /// Registers the server callbacks for the loaded plugin.
        void (*register_server_callbacks)(ServerPluginBuilder& builder);
    };

    /// The public entry point for a server plugin.
    ///
    /// When a plugin is loaded by the server, it will call this entry point to
    /// obtain information about this plugin and about how to register its customization points.
    /// This function needs to be implemented by the plugin, see the example below:
    ///
    /// ```
    /// extern "C" ::clice::PluginInfo LLVM_ATTRIBUTE_WEAK
    /// clice_get_server_plugin_info() {
    ///   return {
    ///     CLICE_PLUGIN_API_VERSION, "MyPlugin", "v0.1", CLICE_PLUGIN_DEF_HASH,
    ///     [](ServerPluginBuilder builder) {  ... }
    ///   };
    /// }
    /// ```
    PluginInfo LLVM_ATTRIBUTE_WEAK clice_get_server_plugin_info();
}

struct ServerRef {
public:
    struct Self;

    ServerRef(Self* self) : self(self) {}

    Self* operator->() const {
        return self;
    }

    Server& server() const;

protected:
    Self* self;
};

/// Defines the library APIs to register callbacks for a plugin.
struct ServerPluginBuilder {
public:
    ServerPluginBuilder(ServerRef server_ref) : server_ref(server_ref) {}

    /// Gets a reference to the server.
    auto get_server_ref() const -> ServerRef {
        return server_ref;
    }

#define CliceServerPluginAPI(METHOD, ...) void METHOD(void* plugin_data, __VA_ARGS__)

    using lifecycle_hook_t = async::Task<> (*)(ServerRef server, void* plugin_data);

    /// Registers a callback to be called when the server is initialized.
    CliceServerPluginAPI(on_initialize, lifecycle_hook_t callback);
    /// Registers a callback to be called when the server is initialized.
    CliceServerPluginAPI(on_initialized, lifecycle_hook_t callback);
    /// Registers a callback to be called when the server is shutdown.
    CliceServerPluginAPI(on_shutdown, lifecycle_hook_t callback);
    /// Registers a callback to be called when the server is exiting.
    CliceServerPluginAPI(on_exit, lifecycle_hook_t callback);
    /// Registers a callback to be called when the server's configuration is changed.
    CliceServerPluginAPI(on_did_change_configuration, lifecycle_hook_t callback);
    using command_handler_t =
        async::Task<llvm::json::Value> (*)(ServerRef server,
                                           void* plugin_data,
                                           llvm::ArrayRef<llvm::StringRef> arguments);
    /// Registers a callback to be called when a command is received from the LSP client.
    CliceServerPluginAPI(register_commmand_handler,
                         llvm::StringRef command,
                         command_handler_t callback);
#undef CliceServerPluginAPI

protected:
    ServerRef server_ref;
};

}  // namespace clice
