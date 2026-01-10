#include "Server/Plugin.h"

#include "Implement.h"

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
        return std::unexpected("Could not load library '" + file_path + "': " + err);
    }

    Plugin P{
        new Self{file_path, library}
    };

    /// `clice_get_server_plugin_info` should be resolved to the definition from the plugin
    /// we are currently loading.
    intptr_t get_details_fn = (intptr_t)library.getAddressOfSymbol("clice_get_server_plugin_info");

    if(!get_details_fn) {
        /// If the symbol isn't found, this is probably a legacy plugin, which is an
        /// error.
        return std::unexpected("Plugin entry point not found in '" + file_path +
                               "'. Is this a clice server plugin?");
    }

    auto info = reinterpret_cast<decltype(clice_get_server_plugin_info)*>(get_details_fn)();

    /// First, we check whether the plugin is compatible with the clice plugin API.
    if(info.api_version != CLICE_PLUGIN_API_VERSION) {
        return std::unexpected("Wrong API version on plugin '" + file_path + "'. Got version " +
                               std::to_string(info.api_version) + ", supported version is " +
                               std::to_string(CLICE_PLUGIN_API_VERSION) + ".");
    }

    /// Then, we safely get definition hash from the plugin, and check if it is consistent with
    /// the expected hash. This ensures that the plugin has consistent declarations with the server.
    std::string definition_hash = info.definition_hash;
    if(definition_hash != CLICE_PLUGIN_DEF_HASH) {
        return std::unexpected("Wrong definition hash on plugin '" + file_path + "'. Got '" +
                               definition_hash + "', expected '" + CLICE_PLUGIN_DEF_HASH + "'.");
    }

    if(!info.register_server_callbacks) {
        return std::unexpected("Empty `register_server_callbacks` function in plugin '" +
                               file_path + "'.");
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

void ServerPluginBuilder::get_server_ref(void* plugin_data, ServerRef& server) {
    server = server_ref;
}

void ServerPluginBuilder::on_initialize(void* plugin_data, lifecycle_hook_t callback) {
    auto server = server_ref;
    server_ref.server().initialize_hooks.push_back(
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
