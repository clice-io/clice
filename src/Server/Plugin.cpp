#include "Server/Plugin.h"

#include "Implement.h"
#include "Server/Utility.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/DynamicLibrary.h"

namespace clice {

Server& ServerRef::server() const {
    return self->server();
}

struct Plugin::Self {
    /// The file path of the plugin.
    std::string file_path;
    /// The dynamic library data of the plugin.
    llvm::sys::DynamicLibrary library;
    /// The name of the plugin.
    std::string name;
    /// The version of the plugin.
    std::string version;
    /// Registers the server callbacks for the loaded plugin.
    void (*register_server_callbacks)(ServerPluginBuilder& builder);
};

std::expected<Plugin, std::string> Plugin::load(const std::string& file_path) {
    std::string err;
    auto library = llvm::sys::DynamicLibrary::getPermanentLibrary(file_path.c_str(), &err);
    if(!library.isValid()) {
        return bail("Could not load library '{}': {}", file_path, err);
    }

    Plugin P{
        /// We currently never destroy plugins, so this is not a memory leak.
        new Self{file_path, library}
    };

    /// `clice_get_server_plugin_info` should be resolved to the definition from the plugin
    /// we are currently loading.
    intptr_t get_details_fn = (intptr_t)library.getAddressOfSymbol("clice_get_server_plugin_info");
    if(!get_details_fn) {
        return bail(
            "The symbol `clice_get_server_plugin_info` is not found in '{}'. Is this a clice server plugin?",
            file_path);
    }

    auto info = reinterpret_cast<decltype(clice_get_server_plugin_info)*>(get_details_fn)();

    /// First, we check whether the plugin is compatible with the clice plugin API.
    if(info.api_version != CLICE_PLUGIN_API_VERSION) {
        return bail("Wrong API version on plugin '{}'. Got version {}. Supported version is {}.",
                    file_path,
                    info.api_version,
                    CLICE_PLUGIN_API_VERSION);
    }

    /// Then, we safely get definition hash from the plugin, and check if it is consistent with
    /// the expected hash. This ensures that the plugin has consistent declarations with the
    /// server.
    std::string definition_hash = info.definition_hash;
    if(plugin_definition_hash.size() != definition_hash.size()) {
        return bail("Wrong definition hash size on plugin '{}'. Got {}, expected {} ({}).",
                    file_path,
                    definition_hash.size(),
                    plugin_definition_hash.size(),
                    plugin_definition_hash);
    }

    /// If there is any non-printable character in the definition hash, this is likely a bug in the
    /// plugin. We cannot even print the `definition_hash` in this case.
    if(std::ranges::any_of(definition_hash, [](char c) { return !std::isprint(c); })) {
        return bail("Corrupt definition hash on plugin '{}'. This is likely a bug in the plugin.",
                    file_path);
    }

    if(definition_hash != CLICE_PLUGIN_DEF_HASH) {
        return bail("Wrong definition hash on plugin '{}'. Got '{}', expected '{}'.",
                    file_path,
                    definition_hash,
                    CLICE_PLUGIN_DEF_HASH);
    }

    /// A plugin must implement the `register_server_callbacks` function.
    if(!info.register_server_callbacks) {
        return bail("Empty `register_server_callbacks` function in plugin '{}'.", file_path);
    }

    P->name = info.name;
    P->version = info.version;
    P->register_server_callbacks = info.register_server_callbacks;

    return P;
}

llvm::StringRef Plugin::file_path() const {
    return self->file_path;
}

llvm::StringRef Plugin::name() const {
    return self->name;
}

llvm::StringRef Plugin::version() const {
    return self->version;
}

using command_handler_t =
    async::Task<llvm::json::Value> (*)(ServerRef server,
                                       const llvm::ArrayRef<llvm::StringRef>& arguments);

void ServerPluginBuilder::on_initialize(void* plugin_data, lifecycle_hook_t callback) {
    auto server = server_ref;
    server_ref.server().initialize_hooks.push_back(
        [=]() -> async::Task<> { co_await callback(server, plugin_data); });
}

void ServerPluginBuilder::on_initialized(void* plugin_data, lifecycle_hook_t callback) {
    auto server = server_ref;
    server_ref.server().initialized_hooks.push_back(
        [=]() -> async::Task<> { co_await callback(server, plugin_data); });
}

void ServerPluginBuilder::on_shutdown(void* plugin_data, lifecycle_hook_t callback) {
    auto server = server_ref;
    server_ref.server().shutdown_hooks.push_back(
        [=]() -> async::Task<> { co_await callback(server, plugin_data); });
}

void ServerPluginBuilder::on_exit(void* plugin_data, lifecycle_hook_t callback) {
    auto server = server_ref;
    server_ref.server().exit_hooks.push_back(
        [=]() -> async::Task<> { co_await callback(server, plugin_data); });
}

void ServerPluginBuilder::on_did_change_configuration(void* plugin_data,
                                                      lifecycle_hook_t callback) {
    auto server = server_ref;
    server_ref.server().did_change_configuration_hooks.push_back(
        [=]() -> async::Task<> { co_await callback(server, plugin_data); });
}

void ServerPluginBuilder::register_commmand_handler(void* plugin_data,
                                                    llvm::StringRef command,
                                                    command_handler_t callback) {
    auto server = server_ref;
    auto [_, inserted] = server_ref.server().command_handlers.try_emplace(
        command,
        [=](llvm::ArrayRef<llvm::StringRef> arguments) -> async::Task<llvm::json::Value> {
            co_return callback(server, plugin_data, arguments);
        });
    if(!inserted) {
        LOG_ERROR("Command handler already registered for command '{}'.", command);
    }
}

}  // namespace clice
