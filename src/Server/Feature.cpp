#include "Feature/Formatting.h"
#include "Server/Server.h"
#include "Server/Convert.h"
#include "Compiler/Compilation.h"
#include "Feature/CodeCompletion.h"
#include "Feature/Hover.h"
#include "Feature/SignatureHelp.h"
#include "Feature/DocumentLink.h"
#include "Feature/DocumentSymbol.h"
#include "Feature/FoldingRange.h"
#include "Feature/SemanticToken.h"
#include "Feature/InlayHint.h"

namespace clice {

auto Server::on_completion(proto::CompletionParams params) -> Result {
    auto path = mapping.to_path(params.textDocument.uri);
    auto opening_file = opening_files.get_or_add(path);

    if(!opening_file->pch_build_task.empty()) {
        co_await opening_file->pch_built_event;
    }

    auto& content = opening_file->content;
    auto offset = to_offset(kind, content, params.position);
    auto& pch = opening_file->pch;
    {
        CommandOptions options;
        options.resource_dir = true;

        /// Set compilation params ... .
        CompilationParams params;
        params.kind = CompilationUnit::Completion;
        params.arguments = database.get_command(path).arguments;
        params.add_remapped_file(path, content);
        params.pch = {pch->path, pch->preamble.size()};
        params.completion = {path, offset};

        co_return co_await async::submit([kind = this->kind, &content, &params] {
            auto items = feature::code_complete(params, {});
            return proto::to_json(kind, content, items);
        });
    }
}

auto Server::on_hover(proto::HoverParams params) -> Result {
    auto path = mapping.to_path(params.textDocument.uri);
    auto opening_file = opening_files.get_or_add(path);
    auto version = opening_file->version;

    auto guard = co_await opening_file->ast_built_lock.try_lock();
    auto ast = opening_file->ast;

    if(opening_file->version != version || !ast) {
        co_return json::Value(nullptr);
    }

    auto offset = to_offset(kind, opening_file->content, params.position);

    // NOTE: Should be initialized somewhere else
    config::HoverOptions opt = {.enable_doxygen_parsing = true,
                                .parse_comment_as_markdown = true,
                                .show_aka = true};

    co_return co_await async::submit([kind = this->kind, offset, &ast, &opt] {
        auto hover = feature::hover(*ast, offset, opt);
        // TODO: Join comment with ast info, build structed text
        proto::Hover result;
        result.contents.kind = "markdown";
        result.contents.value = std::format("{}: {}", hover->kind.name(), hover->name);
        return json::serialize(result);
    });
}

async::Task<json::Value> Server::on_signature_help(proto::SignatureHelpParams params) {
    auto path = mapping.to_path(params.textDocument.uri);
    auto opening_file = opening_files.get_or_add(path);

    if(!opening_file->pch_build_task.empty()) {
        co_await opening_file->pch_built_event;
    }

    auto& content = opening_file->content;
    auto offset = to_offset(kind, content, params.position);
    auto& pch = opening_file->pch;
    {
        CommandOptions options;
        options.resource_dir = true;

        /// Set compilation params ... .
        CompilationParams params;
        params.kind = CompilationUnit::Completion;
        params.arguments = database.get_command(path, options).arguments;
        params.add_remapped_file(path, content);
        params.pch = {pch->path, pch->preamble.size()};
        params.completion = {path, offset};

        co_return co_await async::submit([kind = this->kind, &content, &params] {
            auto help = feature::signature_help(params, {});
            return json::serialize(help);
        });
    }
}

auto Server::on_document_symbol(proto::DocumentSymbolParams params) -> Result {
    auto path = mapping.to_path(params.textDocument.uri);
    auto opening_file = opening_files.get_or_add(path);
    auto version = opening_file->version;

    auto guard = co_await opening_file->ast_built_lock.try_lock();
    auto ast = opening_file->ast;

    if(opening_file->version != version || !ast) {
        co_return json::Value(nullptr);
    }

    llvm::StringRef content = ast->interested_content();
    auto to_range = [&](LocalSourceRange range) {
        auto c = PositionConverter(content, kind);
        auto begin = c.toPosition(range.begin);
        auto end = c.toPosition(range.end);
        return proto::Range{begin, end};
    };

    auto transform = [&to_range](this auto& self,
                                 feature::DocumentSymbol& symbol) -> proto::DocumentSymbol {
        proto::DocumentSymbol result;
        result.name = std::move(symbol.name);
        result.detail = std::move(symbol.detail);
        result.kind = proto::kind_map(symbol.kind.kind());
        result.range = to_range(symbol.range);
        result.selectionRange = to_range(symbol.selectionRange);

        for(auto& child: symbol.children) {
            result.children.emplace_back(self(child));
        }

        return result;
    };

    co_return co_await async::submit([&ast, &transform] {
        auto symbols = feature::document_symbols(*ast);

        std::vector<proto::DocumentSymbol> result;
        for(auto& symbol: symbols) {
            result.emplace_back(transform(symbol));
        }

        return json::serialize(result);
    });
}

auto Server::on_document_link(proto::DocumentLinkParams params) -> Result {
    auto path = mapping.to_path(params.textDocument.uri);
    auto opening_file = opening_files.get_or_add(path);
    auto version = opening_file->version;

    auto guard = co_await opening_file->ast_built_lock.try_lock();
    auto ast = opening_file->ast;

    if(opening_file->version != version || !ast) {
        co_return json::Value(nullptr);
    }

    auto pch_links = opening_file->pch_includes;
    auto mapping = this->mapping;

    co_return co_await async::submit([&, kind = this->kind] {
        auto links = feature::document_links(*ast);
        links.insert(links.begin(), pch_links.begin(), pch_links.end());

        llvm::StringRef content = ast->interested_content();
        PositionConverter converter(content, kind);
        converter.to_positions(links, [](feature::DocumentLink& link) { return link.range; });

        std::vector<proto::DocumentLink> result;
        for(auto& link: links) {
            result.emplace_back(converter.lookup(link.range), mapping.to_uri(link.file));
        }
        return json::serialize(result);
    });
}

auto Server::on_document_format(proto::DocumentFormattingParams params) -> Result {
    auto path = mapping.to_path(params.textDocument.uri);
    auto opening_file = opening_files.get_or_add(path);

    auto content = opening_file->content;
    co_return co_await async::submit([&, kind = this->kind] {
        auto edits = feature::document_format(path, content, std::nullopt);
        /// FIXME: adjust position encoding...
        return json::serialize(edits);
    });
}

auto Server::on_document_range_format(proto::DocumentRangeFormattingParams params) -> Result {
    auto path = mapping.to_path(params.textDocument.uri);
    auto opening_file = opening_files.get_or_add(path);

    auto content = opening_file->content;
    co_return co_await async::submit([&, kind = this->kind] {
        auto begin = to_offset(kind, content, params.range.start);
        auto end = to_offset(kind, content, params.range.end);
        auto edits = feature::document_format(path, content, LocalSourceRange(begin, end));
        /// FIXME: adjust position encoding...
        return json::serialize(edits);
    });
}

async::Task<json::Value> Server::on_folding_range(proto::FoldingRangeParams params) {
    auto path = mapping.to_path(params.textDocument.uri);
    auto opening_file = opening_files.get_or_add(path);
    auto version = opening_file->version;

    auto guard = co_await opening_file->ast_built_lock.try_lock();
    auto ast = opening_file->ast;

    if(opening_file->version != version || !ast) {
        co_return json::Value(nullptr);
    }

    co_return co_await async::submit([&, kind = this->kind] {
        llvm::StringRef content = ast->interested_content();
        auto foldings = feature::folding_ranges(*ast);
        PositionConverter converter(content, kind);
        converter.to_positions(foldings,
                               [](feature::FoldingRange& folding) { return folding.range; });

        std::vector<proto::FoldingRange> result;

        for(auto&& folding: foldings) {
            auto [begin_offset, end_offset] = folding.range;
            auto [begin_line, begin_char] = converter.lookup(begin_offset);
            auto [end_line, end_char] = converter.lookup(end_offset);

            proto::FoldingRange range;
            range.startLine = begin_line;
            range.startCharacter = begin_char;
            range.endLine = end_line;
            range.endCharacter = end_char;
            range.kind = "region";
            range.collapsedText = folding.text;
            result.emplace_back(std::move(range));
        }

        return json::serialize(result);
    });
}

auto Server::on_semantic_token(proto::SemanticTokensParams params) -> Result {
    auto path = mapping.to_path(params.textDocument.uri);
    auto opening_file = opening_files.get_or_add(path);
    auto version = opening_file->version;

    auto guard = co_await opening_file->ast_built_lock.try_lock();
    auto ast = opening_file->ast;

    if(opening_file->version != version || !ast) {
        co_return json::Value(nullptr);
    }

    co_return co_await async::submit([kind = this->kind, &ast] {
        auto tokens = feature::semantic_tokens(*ast);
        return proto::to_json(kind, ast->interested_content(), tokens);
    });
}

auto Server::on_inlay_hint(proto::InlayHintParams params) -> Result {
    auto path = mapping.to_path(params.textDocument.uri);
    auto opening_file = opening_files.get_or_add(path);
    auto version = opening_file->version;

    auto guard = co_await opening_file->ast_built_lock.try_lock();
    auto ast = opening_file->ast;

    if(opening_file->version != version || !ast) {
        co_return json::Value(nullptr);
    }

    co_return co_await async::submit([kind = this->kind, &params, &ast] {
        auto content = ast->interested_content();

        LocalSourceRange range{
            to_offset(kind, content, params.range.start),
            to_offset(kind, content, params.range.end),
        };

        auto hints = feature::inlay_hints(*ast, range, {});

        PositionConverter converter(content, kind);

        std::vector<proto::InlayHint> result;

        for(auto& hint: hints) {
            auto& back = result.emplace_back(converter.toPosition(hint.offset));
            back.label.emplace_back(std::move(hint.parts[0].name));

            /// FIXME: Determine the set of possible kinds; for now, we'll use Type.
            back.kind = proto::InlayHintKind::Type;
        }

        return json::serialize(result);
    });
}

}  // namespace clice
