module clice;

import :command.table;
import :project.command_resolver;
import :project.contexts;
import :project.hosting;
import :project.project;
import :server.master;
import :server.served_project;
import :support.shell;
import :unittests.server.served_project_fixture;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(Contexts) {

TempDir tmp;
ServedProjectFixture fx{tmp.root};
ServedProject& served = *fx.served;
Project& project = served.project;
CommandResolver& commands = served.commands;
MasterServer& server = fx.server;

/// The user chose `choice` for `path_id` in the fixture's project, as
/// switchContext records it.
void choose(Fid path_id, ContextChoice choice) {
    server.choices[path_id] = {.choice = std::move(choice), .project = &served};
}

/// The project serving the directory `sub` of the fixture's, as a nested
/// folder is served. It schedules no save as its choices change: the save
/// runs on the event loop, which these tests do not run.
ServedProject& nested_project() {
    auto nested =
        std::make_shared<ServedProject>(server, CanonicalPath(Spelling::absolute(tmp.path("sub"))));
    server.projects.push_back(nested);
    nested->project.request_flush = nullptr;
    return *nested;
}

/// `owner`'s database holds `choice` for `path_id`, as a project finds its
/// choices when it starts.
void saved(Project& owner, Fid path_id, const ContextChoice& choice) {
    std::pair<Fid, const ContextChoice*> choices[] = {
        {path_id, &choice}
    };
    owner.choices.bytes = serialize_choices(owner, choices);
}

ZEST_CASE(EditorSharesContext) {
    // A background and an editor resolution read the one context hosting
    // synthesized for the header.
    tmp.touch("host.cpp", "struct S {\n#include \"h.h\"\n};\n");
    tmp.touch("h.h", "int member;\n");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("host.cpp"), {"-DHOSTED"}}
    }));
    auto host = fx.intern(tmp.path("host.cpp"));
    auto header = fx.intern(tmp.path("h.h"));
    project.dep_graph.set_includes(host, 0, {{header}});
    project.dep_graph.build_reverse_map();

    // Its trial found it needs the includer's context.
    std::string directory;
    std::vector<std::string> arguments;
    auto first = commands.resolve_command(header, directory, arguments, {.preprocess = true});
    ZASSERT(first.trial);
    auto version = current_dep(project.file_table, header)->version;
    project.hosting.trials.commit(*first.trial,
                                  {.self_contained = false, .text = version, .disk = version},
                                  {});

    auto background = commands.resolve_command(header, directory, arguments);
    ZASSERT(background.source == CommandSource::IncludeGraph);
    ZASSERT(background.host == host);
    ZASSERT(background.synthesized != nullptr);
    ZASSERT(ranges::contains(arguments, background.synthesized->prefix));

    auto resolution = served.resolve_command(header, directory, arguments);
    ZASSERT(resolution.source == CommandSource::IncludeGraph);
    ZASSERT(resolution.synthesized == background.synthesized);
    ZASSERT(!project.choices.dirty);
}

ZEST_CASE(GuessedTracksEditorOnly) {
    // A database change recompiles every open document whose command was
    // guessed; background resolutions must neither set nor clear the mark.
    tmp.touch("lonely.cpp", "");
    tmp.touch("main.cpp", "");
    auto path = tmp.path("lonely.cpp");
    auto file = fx.open(path, "")->path_id;
    std::string directory;
    std::vector<std::string> arguments;

    commands.resolve_command(file, directory, arguments);
    ZASSERT(!fx.entry(file).guessed);

    ZASSERT(served.resolve_command(file, directory, arguments).source == CommandSource::Fallback);
    ZASSERT(fx.entry(file).guessed);

    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, path, {}}
    }));
    commands.resolve_command(file, directory, arguments);
    ZASSERT(fx.entry(file).guessed);
    ZASSERT(served.resolve_command(file, directory, arguments).source == CommandSource::CDBExact);
    ZASSERT(!fx.entry(file).guessed);
}

ZEST_CASE(ChoiceSteersEditorOnly) {
    tmp.touch("main.cpp");
    auto path = tmp.path("main.cpp");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, path, {"-DFIRST"} },
                  {tmp.root, path, {"-DSECOND"}}
    }));

    auto file = fx.intern(path);
    auto candidates = project.cdb.candidate_entries(path);
    ZASSERT(candidates.size() == 2u);
    // Choose the non-default candidate (candidate order is content-decided,
    // so the defines are read back rather than assumed).
    auto define_of = [&](CommandID command) -> llvm::StringRef {
        auto argv = print_argv(project.commands.render_full(command));
        return llvm::StringRef(argv).contains("SECOND") ? "SECOND" : "FIRST";
    };
    auto chosen = candidates.back().command;
    choose(file, ContextChoice{Fid{}, std::nullopt, project.commands.command_hash_hex(chosen)});

    // An editor resolution honors the chosen CDB entry...
    std::string directory;
    std::vector<std::string> arguments;
    served.resolve_command(file, directory, arguments);
    ZASSERT(ranges::contains(arguments, define_of(chosen)));

    // ...but background indexing must never see user choices.
    arguments.clear();
    commands.resolve_command(file, directory, arguments);
    ZASSERT(ranges::contains(arguments, define_of(candidates.front().command)));
}

ZEST_CASE(ChoiceBaseSurvivesRules) {
    tmp.touch("main.cpp");
    auto path = tmp.path("main.cpp");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, path, {"-DFIRST"} },
                  {tmp.root, path, {"-DSECOND"}}
    }));

    auto file = fx.intern(path);
    auto candidates = project.cdb.candidate_entries(path);
    ZASSERT(candidates.size() == 2u);
    auto define_of = [&](CommandID command) -> llvm::StringRef {
        auto argv = print_argv(project.commands.render_full(command));
        return llvm::StringRef(argv).contains("SECOND") ? "SECOND" : "FIRST";
    };
    auto chosen = candidates.back().command;

    // A choice whose applied hash went stale (a rule edit since it was saved)
    // but whose base identity is recorded still selects its candidate...
    choose(file,
           ContextChoice{Fid{},
                         std::nullopt,
                         "0123456789abcdef",
                         project.commands.command_hash_hex(chosen)});
    std::string directory;
    std::vector<std::string> arguments;
    served.resolve_command(file, directory, arguments);
    ZASSERT(ranges::contains(arguments, define_of(chosen)));

    // ...while the same stale hash without a base falls back to the default.
    choose(file, ContextChoice{Fid{}, std::nullopt, "0123456789abcdef", ""});
    arguments.clear();
    served.resolve_command(file, directory, arguments);
    ZASSERT(ranges::contains(arguments, define_of(candidates.front().command)));
}

ZEST_CASE(ValidChoiceStaysActive) {
    tmp.touch("host.cpp", R"(#include "h.h")");
    tmp.touch("h.h");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("host.cpp"), {}}
    }));

    auto host = fx.intern(tmp.path("host.cpp"));
    auto header = fx.intern(tmp.path("h.h"));
    project.dep_graph.set_includes(host, 0, {{header}});
    project.dep_graph.build_reverse_map();
    choose(header, ContextChoice{host, std::nullopt, ""});

    ZASSERT(choice_holds(project, header, server.choice(header)->choice));
    ZASSERT(!server.recheck_choice(header));
    ZASSERT(server.choice(header)->active);
}

ZEST_CASE(GoneOccurrenceGoesInactive) {
    // The host lost the chosen second include of the header: the choice no
    // longer holds, and waits inactive.
    tmp.touch("host.cpp", R"(#include "h.h")");
    tmp.touch("h.h");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("host.cpp"), {}}
    }));

    auto host = fx.intern(tmp.path("host.cpp"));
    auto header = fx.intern(tmp.path("h.h"));
    project.dep_graph.set_includes(host, 0, {{header}});
    project.dep_graph.build_reverse_map();
    choose(header, ContextChoice{host, 1, ""});

    // No tree tells how often the host's compile enters the header yet:
    // the choice holds until one does.
    ZASSERT(!server.recheck_choice(header));
    ZASSERT(server.choice(header)->active);
    take_tree(project, tree_request(project, host, project.build.commands(host).front()));
    ZASSERT(!choice_holds(project, header, server.choice(header)->choice));
    ZASSERT(server.recheck_choice(header));
    ZASSERT(!server.choice(header)->active);
}

ZEST_CASE(StaleChoiceGoesInactive) {
    tmp.touch("host.cpp");
    tmp.touch("h.h");
    tmp.touch("main.cpp");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));

    auto host = fx.intern(tmp.path("host.cpp"));
    auto header = fx.intern(tmp.path("h.h"));
    auto main = fx.intern(tmp.path("main.cpp"));

    // A host choice whose CDB entry disappeared while the server was down.
    choose(header, ContextChoice{host, std::nullopt, ""});
    ZASSERT(server.recheck_choice(header));
    ZASSERT(!server.choice(header)->active);

    // A command choice whose hash matches no current CDB entry.
    choose(main, ContextChoice{Fid{}, std::nullopt, "deadbeef"});
    ZASSERT(server.recheck_choice(main));
    ZASSERT(!server.choice(main)->active);
}

ZEST_CASE(HostReturnReactivates) {
    // A choice whose host leaves the build stays, inactive, and applies
    // again once the host is back.
    tmp.touch("host.cpp", R"(#include "h.h")");
    tmp.touch("h.h");
    tmp.touch("main.cpp");
    auto with_host = build_cdb_json({
        {tmp.root, tmp.path("host.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, with_host);

    auto host = fx.intern(tmp.path("host.cpp"));
    auto header = fx.intern(tmp.path("h.h"));
    project.dep_graph.set_includes(host, 0, {{header}});
    project.dep_graph.build_reverse_map();
    choose(header, ContextChoice{host, std::nullopt, ""});
    ZASSERT(!server.recheck_choice(header));

    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    ZASSERT(server.recheck_choice(header));
    auto* entry = server.choice(header);
    ZASSERT(entry != nullptr);
    ZASSERT(!entry->active);
    ZASSERT(entry->choice.host_path_id == host);
    ZASSERT(served.active_choice(header) == nullptr);

    write_cdb(tmp, project.cdb, with_host);
    ZASSERT(server.recheck_choice(header));
    ZASSERT(server.choice(header)->active);
    ZASSERT(served.active_choice(header) == &server.choice(header)->choice);
}

ZEST_CASE(DeeperChoiceWins) {
    // Of two projects that saved a choice for one file, the deeper root's
    // stands; the other's leaves its database at its next save.
    auto& inner = nested_project();
    auto header = fx.intern(tmp.path("sub/h.h"));
    saved(project, header, ContextChoice{.command_hash = "outer"});
    saved(inner.project, header, ContextChoice{.command_hash = "inner"});

    server.load_choices(served);
    ZASSERT(server.choice(header)->project == &served);
    server.load_choices(inner);
    auto* entry = server.choice(header);
    ZASSERT(entry->project == &inner);
    ZASSERT(entry->choice.command_hash == "inner");
    ZASSERT(project.choices.dirty);
    ZASSERT(parse_choices(project, project.choices.bytes).empty());
    ZASSERT(!inner.project.choices.dirty);
}

ZEST_CASE(ShallowerChoiceDropped) {
    // The deeper root's choice stands whichever project starts first.
    auto& inner = nested_project();
    auto header = fx.intern(tmp.path("sub/h.h"));
    saved(project, header, ContextChoice{.command_hash = "outer"});
    saved(inner.project, header, ContextChoice{.command_hash = "inner"});

    server.load_choices(inner);
    server.load_choices(served);
    auto* entry = server.choice(header);
    ZASSERT(entry->project == &inner);
    ZASSERT(entry->choice.command_hash == "inner");
    ZASSERT(project.choices.dirty);
    ZASSERT(parse_choices(project, project.choices.bytes).empty());
    ZASSERT(!inner.project.choices.dirty);
}

};  // ZEST_SUITE(Contexts)

}  // namespace
}  // namespace clice::testing
