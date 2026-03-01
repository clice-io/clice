#include "server/compilation_service.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "compile/command.h"
#include "eventide/language/uri.h"
#include "support/filesystem.h"

namespace clice::server {

namespace {

namespace language = eventide::language;
namespace fs_std = std::filesystem;

struct CompilationDatabaseCacheEntry {
    std::shared_ptr<CompilationDatabase> database;
    fs_std::file_time_type write_time{};
    bool loaded = false;
};

auto normalized_path(fs_std::path path) -> std::string {
    std::error_code ec;
    auto canonical = fs_std::weakly_canonical(path, ec);
    if(!ec) {
        return canonical.string();
    }
    return path.lexically_normal().string();
}

auto resolve_source_path(std::string_view uri) -> std::expected<std::string, std::string> {
    if(auto parsed = language::URI::parse(uri)) {
        if(!parsed->is_file()) {
            return std::unexpected("worker only supports file:// document uri");
        }

        auto file_path = parsed->file_path();
        if(!file_path) {
            return std::unexpected(file_path.error());
        }
        return normalized_path(*file_path);
    }

    fs_std::path candidate(uri);
    if(candidate.is_absolute()) {
        return normalized_path(std::move(candidate));
    }

    return std::unexpected("invalid document uri: " + std::string(uri));
}

auto find_compilation_database_path(std::string_view source_path) -> std::optional<std::string> {
    fs_std::path current = fs_std::path(source_path).parent_path();
    std::error_code ec;

    while(!current.empty()) {
        auto candidate = current / "compile_commands.json";
        if(fs_std::exists(candidate, ec) && !ec) {
            return normalized_path(std::move(candidate));
        }
        ec.clear();

        auto parent = current.parent_path();
        if(parent == current) {
            break;
        }
        current = std::move(parent);
    }

    return std::nullopt;
}

auto is_cpp_like_file(std::string_view path) -> bool {
    constexpr std::array<std::string_view, 13> cpp_ext = {
        ".cc",
        ".cpp",
        ".cxx",
        ".c++",
        ".cp",
        ".cppm",
        ".ixx",
        ".hpp",
        ".hxx",
        ".hh",
        ".h++",
        ".ipp",
        ".tpp",
    };

    auto extension = fs_std::path(path).extension().string();
    for(auto known: cpp_ext) {
        if(extension == known) {
            return true;
        }
    }
    return false;
}

}  // namespace

struct CompilationService::Impl {
    static auto same_recipe(const WorkerGetCompileRecipeResult& lhs,
                            const WorkerGetCompileRecipeResult& rhs) -> bool {
        return lhs.source_path == rhs.source_path &&
               lhs.arguments_from_database == rhs.arguments_from_database &&
               lhs.directory == rhs.directory && lhs.arguments == rhs.arguments;
    }

    static auto make_recipe_from_context(std::string_view source_path,
                                         const CompilationContext& context,
                                         bool arguments_from_database)
        -> WorkerGetCompileRecipeResult {
        WorkerGetCompileRecipeResult recipe;
        recipe.source_path = std::string(source_path);
        recipe.arguments_from_database = arguments_from_database;
        recipe.directory = context.directory.str();
        recipe.arguments.reserve(context.arguments.size());
        for(auto argument: context.arguments) {
            recipe.arguments.emplace_back(argument);
        }
        return recipe;
    }

    auto build_recipe_from_database(CompilationDatabase& database,
                                    std::string_view source_path) const
        -> std::optional<WorkerGetCompileRecipeResult> {
        CommandOptions options;
        options.resource_dir = !fs::resource_dir.empty();
        options.query_toolchain = false;
        options.suppress_logging = true;

        auto context = database.lookup(source_path, options);
        if(context.arguments.empty() || context.directory.empty()) {
            return std::nullopt;
        }

        return make_recipe_from_context(source_path, context, true);
    }

    auto build_recipe_from_configured_database(std::string_view source_path) const
        -> std::optional<WorkerGetCompileRecipeResult> {
        if(!configured_database) {
            return std::nullopt;
        }

        return build_recipe_from_database(*configured_database, source_path);
    }

    auto assign_revision(WorkerGetCompileRecipeResult recipe) -> WorkerGetCompileRecipeResult {
        auto cache_iter = recipes.find(recipe.source_path);
        if(cache_iter != recipes.end() && same_recipe(cache_iter->second, recipe)) {
            auto cached = cache_iter->second;
            cached.unchanged = false;
            return cached;
        }

        recipe.revision = ++next_revision;
        if(recipe.revision == 0) {
            recipe.revision = ++next_revision;
        }
        recipe.unchanged = false;
        recipes[recipe.source_path] = recipe;
        return recipe;
    }

    auto build_recipe(std::string_view source_path)
        -> std::expected<WorkerGetCompileRecipeResult, std::string> {
        if(auto configured = build_recipe_from_configured_database(source_path)) {
            return std::move(*configured);
        }

        auto db_path = find_compilation_database_path(source_path);
        if(!db_path) {
            return make_fallback_recipe(source_path);
        }

        auto database = load_compilation_database(*db_path);
        if(!database) {
            return make_fallback_recipe(source_path);
        }

        if(auto recipe = build_recipe_from_database(**database, source_path)) {
            return std::move(*recipe);
        }

        return make_fallback_recipe(source_path);
    }

    auto make_fallback_recipe(std::string_view source_path) const -> WorkerGetCompileRecipeResult {
        WorkerGetCompileRecipeResult recipe;
        recipe.source_path = std::string(source_path);
        recipe.arguments_from_database = false;
        recipe.directory = fs_std::path(source_path).parent_path().string();
        if(recipe.directory.empty()) {
            recipe.directory = ".";
        }

        recipe.arguments.emplace_back(is_cpp_like_file(source_path) ? "clang++" : "clang");
        if(is_cpp_like_file(source_path)) {
            recipe.arguments.emplace_back("-std=c++20");
        }
        if(!fs::resource_dir.empty()) {
            recipe.arguments.emplace_back("-resource-dir");
            recipe.arguments.emplace_back(fs::resource_dir);
        }
        recipe.arguments.emplace_back(source_path);
        return recipe;
    }

    auto load_compilation_database(const std::string& raw_path)
        -> std::expected<std::shared_ptr<CompilationDatabase>, std::string> {
        auto path = normalized_path(raw_path);

        std::error_code ec;
        auto write_time = fs_std::last_write_time(path, ec);
        if(ec) {
            return std::unexpected("failed to stat compilation database: " + path);
        }

        auto cache_iter = databases.find(path);
        if(cache_iter != databases.end() && cache_iter->second.loaded &&
           cache_iter->second.write_time == write_time) {
            return cache_iter->second.database;
        }

        auto database = std::make_shared<CompilationDatabase>();
        database->load_compile_database(path);

        databases[path] = CompilationDatabaseCacheEntry{
            .database = database,
            .write_time = write_time,
            .loaded = true,
        };
        return database;
    }

    void set_compile_commands_paths(const std::vector<std::string>& paths) {
        configured_database_paths.clear();
        configured_database_paths.reserve(paths.size());

        for(const auto& path: paths) {
            if(path.empty()) {
                continue;
            }

            std::error_code ec;
            fs_std::path candidate(path);
            if(fs_std::is_directory(candidate, ec) && !ec) {
                candidate /= "compile_commands.json";
            }
            ec.clear();
            configured_database_paths.emplace_back(normalized_path(std::move(candidate)));
        }

        std::sort(configured_database_paths.begin(), configured_database_paths.end());
        configured_database_paths.erase(
            std::unique(configured_database_paths.begin(), configured_database_paths.end()),
            configured_database_paths.end());

        configured_database.reset();
        recipes.clear();

        if(configured_database_paths.empty()) {
            return;
        }

        auto merged_database = std::make_shared<CompilationDatabase>();
        std::size_t loaded_count = 0;
        for(const auto& db_path: configured_database_paths) {
            std::error_code ec;
            if(!fs_std::exists(db_path, ec) || ec) {
                continue;
            }

            merged_database->load_compile_database(db_path);
            loaded_count += 1;
        }

        if(loaded_count == 0) {
            return;
        }

        configured_database = std::move(merged_database);
    }

    auto resolve_recipe(const WorkerGetCompileRecipeParams& params)
        -> std::expected<WorkerGetCompileRecipeResult, std::string> {
        auto source_path = resolve_source_path(params.uri);
        if(!source_path) {
            return std::unexpected(std::move(source_path.error()));
        }

        auto recipe = build_recipe(*source_path);
        if(!recipe) {
            return std::unexpected(std::move(recipe.error()));
        }
        auto resolved = assign_revision(std::move(*recipe));

        if(params.known_revision != 0 && params.known_revision == resolved.revision &&
           params.known_source_path == resolved.source_path) {
            return WorkerGetCompileRecipeResult{
                .source_path = resolved.source_path,
                .revision = resolved.revision,
                .unchanged = true,
            };
        }

        return resolved;
    }

    std::unordered_map<std::string, CompilationDatabaseCacheEntry> databases;
    std::vector<std::string> configured_database_paths;
    std::shared_ptr<CompilationDatabase> configured_database;
    std::unordered_map<std::string, WorkerGetCompileRecipeResult> recipes;
    std::uint64_t next_revision = 0;
};

CompilationService::CompilationService() : impl(std::make_unique<Impl>()) {}

CompilationService::~CompilationService() = default;

CompilationService::CompilationService(CompilationService&&) noexcept = default;

CompilationService& CompilationService::operator=(CompilationService&&) noexcept = default;

void CompilationService::set_compile_commands_paths(const std::vector<std::string>& paths) {
    impl->set_compile_commands_paths(paths);
}

auto CompilationService::resolve_recipe(const WorkerGetCompileRecipeParams& params)
    -> std::expected<WorkerGetCompileRecipeResult, std::string> {
    return impl->resolve_recipe(params);
}

}  // namespace clice::server
