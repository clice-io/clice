#include "server/worker_runtime.h"

#include <algorithm>

#include "feature/feature.h"

namespace clice::server {

void CompileCommand::finalize() {
    arguments.clear();
    arguments.reserve(owned_arguments.size());
    for(auto& argument: owned_arguments) {
        arguments.push_back(argument.c_str());
    }
}

auto make_compile_command(const WorkerGetCompileRecipeResult& recipe)
    -> std::expected<CompileCommand, std::string> {
    if(recipe.source_path.empty()) {
        return std::unexpected("compile recipe source path is empty");
    }
    if(recipe.arguments.empty()) {
        return std::unexpected("compile recipe arguments are empty");
    }

    CompileCommand command;
    command.arguments_from_database = recipe.arguments_from_database;
    command.directory = recipe.directory;
    command.owned_arguments = recipe.arguments;
    command.finalize();
    return command;
}

auto resolve_compile_command(eventide::jsonrpc::Peer& peer,
                             std::string_view uri,
                             RecipeState& recipe_state)
    -> eventide::task<std::expected<CompileCommand, std::string>> {
    auto recipe = co_await peer.send_request(WorkerGetCompileRecipeParams{
        .uri = std::string(uri),
        .known_revision = recipe_state.recipe_revision,
        .known_source_path = recipe_state.source_path,
    });
    if(!recipe) {
        co_return std::unexpected(std::move(recipe.error()));
    }

    if(recipe->unchanged) {
        if(!recipe_state.recipe) {
            co_return std::unexpected(
                "master returned unchanged compile recipe without local cache");
        }

        recipe_state.source_path = recipe_state.recipe->source_path;
        recipe_state.recipe_revision = recipe_state.recipe->revision;

        auto command = make_compile_command(*recipe_state.recipe);
        if(!command) {
            co_return std::unexpected(std::move(command.error()));
        }
        co_return std::move(*command);
    }

    recipe_state.recipe = std::move(*recipe);
    recipe_state.source_path = recipe_state.recipe->source_path;
    recipe_state.recipe_revision = recipe_state.recipe->revision;

    auto command = make_compile_command(*recipe_state.recipe);
    if(!command) {
        co_return std::unexpected(std::move(command.error()));
    }
    co_return std::move(*command);
}

auto to_offset_utf16(std::string_view text, int line, int character) -> std::uint32_t {
    auto safe_line = static_cast<rpc::uinteger>(std::max(line, 0));
    auto safe_character = static_cast<rpc::uinteger>(std::max(character, 0));
    feature::PositionMapper mapper(text, feature::PositionEncoding::UTF16);
    return mapper.to_offset(rpc::Position{
        .line = safe_line,
        .character = safe_character,
    });
}

}  // namespace clice::server
