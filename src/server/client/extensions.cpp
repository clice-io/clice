module clice;

import :feature.code_actions;
import :feature.counterparts;
import :feature.protocol;
import :feature.query;
import :server.client;
import :server.extension;
import :server.master;
import :server.served_project;
import :settings.configuration;
import :support.json;
import :support.logging;

namespace clice {

/// A source action running `command` with `params`, the parameters of the
/// request it stands for.
template <typename Params>
static protocol::CodeAction source_action(std::string title,
                                          std::string_view kind,
                                          std::string_view command,
                                          const Params& params) {
    auto argument = codec::dyn::to_dyn<ipc::lsp_config>(params);
    assert(argument && "the parameters of a request encode");
    return {
        .title = title,
        .kind = protocol::CodeActionKind(kind),
        .command = protocol::Command{.title = std::move(title),
                                     .command = std::string(command),
                                     .arguments = std::vector{std::move(*argument)}},
    };
}

std::vector<protocol::CodeAction> context_actions(llvm::StringRef uri,
                                                  const ext::CurrentContextResult& current,
                                                  const ext::QueryContextResult& listing) {
    std::vector<protocol::CodeAction> actions;
    if(listing.total <= 1 && current.automatic) {
        return actions;
    }
    for(auto& item: listing.contexts) {
        if(current.context && item.uri == current.context->uri &&
           item.occurrence == current.context->occurrence &&
           item.command_hash == current.context->command_hash) {
            continue;
        }
        auto title = item.uri == uri ? std::format("Compile with the command {}", item.label)
                                     : std::format("Compile as included by {}", item.label);
        actions.push_back(source_action(std::move(title),
                                        context_action_kind,
                                        switch_context_command,
                                        ext::SwitchContextParams{.uri = uri.str(),
                                                                 .context_uri = item.uri,
                                                                 .occurrence = item.occurrence,
                                                                 .command_hash = item.command_hash,
                                                                 .epoch = listing.epoch}));
    }
    if(!current.automatic) {
        actions.push_back(source_action("Compile in the context clice picks",
                                        context_action_kind,
                                        reset_context_command,
                                        ext::ResetContextParams{.uri = uri.str()}));
    }
    return actions;
}

std::vector<protocol::CodeAction> configuration_actions(llvm::StringRef uri,
                                                        const ext::ListConfigurationsResult& list) {
    std::vector<protocol::CodeAction> actions;
    auto next = list.selected.empty() ? list.active : list.selected;
    for(auto& name: list.configurations) {
        if(name != next) {
            actions.push_back(
                source_action(std::format("Use build configuration {} from the next start", name),
                              configuration_action_kind,
                              switch_configuration_command,
                              ext::SwitchConfigurationParams{.name = name, .uri = uri.str()}));
        }
    }
    return actions;
}

ext::ListConfigurationsResult list_configurations(const Project& project) {
    ext::ListConfigurationsResult result;
    for(auto tag: project.settings.configurations()) {
        result.configurations.push_back(tag.str());
    }
    result.active = project.build.active_configuration().str();
    result.selected = read_selected_configuration(project.settings.project.cache_dir);
    result.default_configuration = project.settings.fallback_configuration().str();
    return result;
}

ext::SwitchConfigurationResult select_configuration(const Project& project,
                                                    llvm::StringRef name,
                                                    llvm::StringRef pinned) {
    auto refuse = [&](std::string reason) {
        LOG_WARN("Cannot select configuration {}: {}", name, reason);
        return ext::SwitchConfigurationResult{.reason = std::move(reason)};
    };
    if(!project.settings.declares(name)) {
        return refuse(std::format("no rule declares the configuration {}", name));
    }
    if(project.settings.declares(pinned)) {
        return refuse(
            std::format("the server runs with --configuration {}, which pins it", pinned));
    }
    if(auto written = write_selected_configuration(project.settings.project.cache_dir, name);
       !written) {
        return refuse(std::format("the selection cannot be saved: {}", written.error().message()));
    }
    LOG_INFO("Selected configuration {}; it becomes active at the next start", name);
    return {.success = true};
}

kota::task<ext::SwitchContextResult>
    LSPClient::switch_context(const ext::SwitchContextParams& params) {
    auto path = uri_to_path(params.uri);
    auto context_path = uri_to_path(params.context_uri);
    if(!path || !context_path) {
        co_return ext::SwitchContextResult{};
    }
    co_return co_await server.switch_context(server.files.intern(*path),
                                             server.files.intern(*context_path),
                                             params);
}

ext::SwitchContextResult LSPClient::reset_context(const ext::ResetContextParams& params) {
    auto path = uri_to_path(params.uri);
    if(!path) {
        return {};
    }
    return server.reset_context(server.files.intern(*path));
}

std::shared_ptr<ServedProject> LSPClient::configured(const std::optional<std::string>& uri) {
    if(uri) {
        return resolve_uri(*uri).project;
    }
    auto& projects = server.projects;
    auto it =
        ranges::find_if(projects, [](auto& project) { return !project->project.root.empty(); });
    return it != projects.end() ? *it : projects.front();
}

ext::SwitchConfigurationResult
    LSPClient::switch_configuration(const ext::SwitchConfigurationParams& params) {
    auto project = configured(params.uri);
    auto result =
        select_configuration(project->project, params.name, server.requested_configuration);
    if(!result.success) {
        return result;
    }
    // A selection replaces the last one; back on the active configuration,
    // there is nothing left to apply.
    auto topic = std::format("selection of {}", project->project.root.str());
    if(params.name == project->project.build.active_configuration()) {
        server.withdraw_restart(topic);
    } else {
        server.ask_restart({
            .file = project->settings_file,
            .message = std::format("Build configuration {} selected", params.name),
            .topic = std::move(topic),
        });
    }
    return result;
}

/// The one argument of `params`, the parameters of the request its command
/// stands for.
template <typename Params>
static std::expected<Params, ipc::Error> argument_of(const protocol::ExecuteCommandParams& params) {
    Params decoded;
    if(!params.arguments || params.arguments->size() != 1 ||
       !codec::dyn::from_dyn<ipc::lsp_config>(params.arguments->front(), decoded)) {
        return std::unexpected(ipc::Error{
            protocol::ErrorCode::InvalidParams,
            std::format("{} takes one argument, the parameters of its request", params.command)});
    }
    return decoded;
}

LSPClient::RawResult LSPClient::execute_command(protocol::ExecuteCommandParams params) {
    // A client with no UI for these tells the user nothing of the outcome;
    // the server does.
    auto tell = [this](protocol::MessageType type, std::string message) {
        peer.send_notification(
            protocol::ShowMessageParams{.type = type, .message = std::move(message)});
    };
    if(params.command == switch_context_command || params.command == reset_context_command) {
        ext::SwitchContextResult result;
        if(params.command == switch_context_command) {
            auto argument = argument_of<ext::SwitchContextParams>(params);
            if(!argument) {
                co_await kota::fail(std::move(argument.error()));
            }
            result = co_await switch_context(*argument);
        } else {
            auto argument = argument_of<ext::ResetContextParams>(params);
            if(!argument) {
                co_await kota::fail(std::move(argument.error()));
            }
            result = reset_context(*argument);
        }
        if(result.stale) {
            tell(protocol::MessageType::Warning,
                 "clice: the contexts changed meanwhile; ask for the actions again");
        } else if(!result.success) {
            tell(protocol::MessageType::Warning, "clice: cannot switch the compilation context");
        }
        co_return to_raw(result);
    }
    if(params.command == switch_configuration_command) {
        auto argument = argument_of<ext::SwitchConfigurationParams>(params);
        if(!argument) {
            co_await kota::fail(std::move(argument.error()));
        }
        auto result = switch_configuration(*argument);
        if(!result.success) {
            tell(protocol::MessageType::Warning,
                 std::format("clice: cannot switch the build configuration: {}", *result.reason));
        }
        co_return to_raw(result);
    }
    co_await kota::fail(ipc::Error{protocol::ErrorCode::InvalidParams,
                                   std::format("no command {}", params.command)});
}

void LSPClient::register_extensions() {
    on_request<ext::QueryContextParams>(
        "clice/queryContext",
        [this](RequestContext& ctx, const ext::QueryContextParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto path = uri_to_path(params.uri);
            if(!path) {
                co_return to_raw(ext::QueryContextResult{});
            }
            co_return to_raw(this->server.query_contexts(this->server.files.intern(*path), params));
        });

    on_request<ext::CurrentContextParams>(
        "clice/currentContext",
        [this](RequestContext& ctx, const ext::CurrentContextParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto document = resolve_uri(params.uri);
            if(document.project->broken()) {
                co_return to_raw(ext::CurrentContextResult{});
            }
            auto path_id = document.session ? document.path_id : Fid();
            co_return to_raw(co_await this->server.current_context(path_id));
        });

    on_request<ext::SwitchContextParams>(
        "clice/switchContext",
        [this](RequestContext& ctx, const ext::SwitchContextParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            co_return to_raw(co_await switch_context(params));
        });

    on_request<ext::ResetContextParams>(
        "clice/resetContext",
        [this](RequestContext& ctx, const ext::ResetContextParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            co_return to_raw(reset_context(params));
        });

    on_request<ext::ListConfigurationsParams>(
        "clice/listConfigurations",
        [this](RequestContext& ctx, const ext::ListConfigurationsParams& params) -> RawResult {
            co_return to_raw(list_configurations(configured(params.uri)->project));
        });

    on_request<ext::SwitchConfigurationParams>(
        "clice/switchConfiguration",
        [this](RequestContext& ctx, const ext::SwitchConfigurationParams& params) -> RawResult {
            co_return to_raw(switch_configuration(params));
        });

    on_request<protocol::ExecuteCommandParams>(
        [this](RequestContext& ctx, const protocol::ExecuteCommandParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            co_return co_await execute_command(params).or_fail();
        });

    on_request<ext::CounterpartsParams>(
        "clice/counterparts",
        [this](RequestContext& ctx, const ext::CounterpartsParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto resolved = resolve_uri(params.uri);
            ext::CounterpartsResult result;
            if(resolved.path.empty()) {
                co_return to_raw(result);
            }
            auto& project = resolved.project->project;
            feature::query::Context context{
                .query = resolved.project->project.index_query,
                .root = project.root,
                .documentation = project.settings.documentation,
            };
            auto found = feature::counterparts(context,
                                               project.dep_graph,
                                               project.build.members(),
                                               Spelling::absolute(resolved.path));
            // A buffer not on disk yet has no counterparts to offer.
            if(!found) {
                co_return to_raw(result);
            }
            for(auto& candidate: found->candidates) {
                result.candidates.push_back({.uri = feature::to_uri(candidate.path),
                                             .reasons = std::move(candidate.reasons)});
            }
            if(found->preferred) {
                result.preferred = feature::to_uri(*found->preferred);
            }
            co_return to_raw(result);
        });
}

}  // namespace clice
