module clice;

import :command.argument;
import :command.table;
import :project.command;
import :project.project;
import :server.master;
import :server.served_project;
import :settings.rules;
import :settings.settings;
import :unittests.server.served_project_fixture;
import :unittests.utils.cdb_helper;
import :unittests.utils.project_fixture;
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
    ServedProjectFixture::hold_flushes(*nested);
    return *nested;
}

/// `owner`'s database holds `choice` for `path_id`, as a project finds its
/// choices when it starts.
void saved(Project& owner, Fid path_id, const ContextChoice& choice) {
    std::pair<Fid, const ContextChoice*> choices[] = {
        {path_id, &choice}
    };
    owner.save_choices(choices);
    owner.choices.dirty = false;
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

    // Its self-containment check found it needs the includer's context.
    project.take_tree_in_process(project.tree_request(host, project.candidates(host).front()));
    auto first = project.resolve_command(header);
    ZASSERT(first.self_containment);
    auto version = current_input(project.file_table, header)->version;
    project.hosting.self_containment.commit(
        *first.self_containment,
        {.self_contained = false, .text = version, .disk = version},
        {});

    auto background = project.resolve_command(header);
    ZASSERT(background.ref.source == CommandSource::Host);
    ZASSERT(background.host == host);
    ZASSERT(background.synthesized != nullptr);
    ZASSERT(ranges::contains(background.arguments, background.synthesized->prefix));

    auto resolution = project.resolve_command(header, served.command_request(header));
    ZASSERT(resolution.ref.source == CommandSource::Host);
    ZASSERT(resolution.synthesized == background.synthesized);
    ZASSERT(!project.choices.dirty);
}

ZEST_CASE(MovedCommandRecompiles) {
    // A database change recompiles an open document whose command moved —
    // one that borrowed a command and gained its own entry — and no other.
    tmp.touch("lonely.cpp", "");
    tmp.touch("main.cpp", "");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    auto lonely = fx.open(tmp.path("lonely.cpp"), "")->path_id;
    auto main = fx.open(tmp.path("main.cpp"), "")->path_id;
    // The documents compiled under what the editor resolves for them.
    auto compiled = [&](Fid file) {
        auto resolution = project.resolve_command(file, served.command_request(file));
        fx.entry(file).latest =
            std::make_shared<DocumentCompile>(DocumentCompile{.command = resolution.ref.command});
        return resolution.ref.source;
    };
    ZASSERT(compiled(lonely) == CommandSource::Borrowed);
    ZASSERT(compiled(main) == CommandSource::Database);

    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"),   {}       },
                  {tmp.root, tmp.path("lonely.cpp"), {"-DOWN"}},
    }));
    fx.commands_changed({.added = {lonely}});
    ZASSERT(fx.generation(lonely) != 0);
    ZASSERT(fx.generation(main) == 0);
    ZASSERT(project.resolve_command(lonely, served.command_request(lonely)).ref.source ==
            CommandSource::Database);
}

ZEST_CASE(MovedHostRecompiles) {
    // A header borrowing a unit's command gains a host compiling it under
    // that very command: a member recorded beside it includes it. What the
    // header compiles in moved, and it recompiles.
    tmp.touch("lib/x.cpp", "int x() { return 0; }\n");
    tmp.touch("lib/frag.h", "Ctx use();\n");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("lib/x.cpp"), {}}
    }));
    auto header = fx.open(tmp.path("lib/frag.h"), "Ctx use();\n")->path_id;
    auto before = project.select_command(header, {.open = true});
    ZASSERT(before.ref.source == CommandSource::Borrowed);
    fx.entry(header).latest = std::make_shared<DocumentCompile>(
        DocumentCompile{.host = before.host, .command = before.ref.command});

    tmp.touch("lib/new.cpp", "struct Ctx {};\n#include \"frag.h\"\n");
    auto member = fx.intern(tmp.path("lib/new.cpp"));
    ZASSERT(project.record_provisional(member));
    fx.commands_changed({});

    auto after = project.select_command(header, {.open = true});
    ZASSERT(after.host == member);
    ZASSERT(after.ref.command == before.ref.command);
    ZASSERT(fx.generation(header) != 0);
}

ZEST_CASE(StaleHostKeepsEntry) {
    // A unit whose chosen host no longer enters it compiles under its own
    // entry, whatever other host ranks first.
    tmp.touch("part.cpp", "int part;\n");
    tmp.touch("chosen.cpp", "");
    tmp.touch("ranked.cpp", "#include \"part.cpp\"\n");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("part.cpp"),   {"-DOWN"}   },
                  {tmp.root, tmp.path("chosen.cpp"), {"-DCHOSEN"}},
                  {tmp.root, tmp.path("ranked.cpp"), {"-DRANKED"}},
    }));
    auto part = fx.intern(tmp.path("part.cpp"));
    auto chosen = fx.intern(tmp.path("chosen.cpp"));
    auto ranked = fx.intern(tmp.path("ranked.cpp"));
    project.dep_graph.set_includes(ranked, 0, {{part}});
    project.dep_graph.build_reverse_map();
    for(auto host: {chosen, ranked}) {
        project.take_tree_in_process(project.tree_request(host, project.candidates(host).front()));
    }

    ContextChoice choice{.host = chosen};
    auto resolution = project.resolve_command(part, {.choice = &choice});
    ZEXPECT(resolution.ref.source == CommandSource::Database);
    ZEXPECT(ranges::contains(resolution.arguments, "OWN"));
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
        auto argv = join_command(project.commands.render_full(command));
        return llvm::StringRef(argv).contains("SECOND") ? "SECOND" : "FIRST";
    };
    auto chosen = candidates.back().command;
    choose(file, ContextChoice{Fid{}, std::nullopt, project.commands.command_hash_hex(chosen)});

    // An editor resolution honors the chosen CDB entry...
    ZASSERT(ranges::contains(project.resolve_command(file, served.command_request(file)).arguments,
                             define_of(chosen)));

    // ...but background indexing must never see user choices.
    ZASSERT(ranges::contains(project.resolve_command(file).arguments,
                             define_of(candidates.front().command)));
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
        auto argv = join_command(project.commands.render_full(command));
        return llvm::StringRef(argv).contains("SECOND") ? "SECOND" : "FIRST";
    };
    auto chosen = candidates.back().command;

    // A choice of the command as written still selects its candidate after
    // a rule edits every command since it was made...
    choose(file, ContextChoice{Fid{}, std::nullopt, project.commands.command_hash_hex(chosen)});
    project.settings.rules.push_back(Rule{.append = {"-DRULED"}});
    finish_settings(project);
    auto resolution = project.resolve_command(file, served.command_request(file));
    ZASSERT(ranges::contains(resolution.arguments, "RULED"));
    ZASSERT(ranges::contains(resolution.arguments, define_of(chosen)));

    // ...while one naming no command falls back to the default.
    choose(file, ContextChoice{Fid{}, std::nullopt, "0123456789abcdef"});
    ZASSERT(ranges::contains(project.resolve_command(file, served.command_request(file)).arguments,
                             define_of(candidates.front().command)));
}

ZEST_CASE(ChoiceNamesWrittenCommand) {
    // A choice names the command as the database writes it: the database
    // changing only a flag a rule removes leaves the command the file
    // compiles under as it was, and ends the choice all the same.
    tmp.touch("main.cpp");
    auto path = tmp.path("main.cpp");
    project.settings.rules.push_back(Rule{.remove = {"-W*"}});
    finish_settings(project);
    auto entries = [&](llvm::StringRef warning) {
        write_cdb(tmp,
                  project.cdb,
                  build_cdb_json({
                      {tmp.root, path, {"-DFIRST", warning.str()}},
                      {tmp.root, path, {"-DSECOND"}              },
        }));
    };
    auto file = fx.intern(path);
    auto first = [&] {
        auto own = project.candidates(file);
        auto it = ranges::find_if(own, [&](const Candidate& candidate) {
            return llvm::StringRef(join_command(project.commands.render_full(candidate.base)))
                .contains("FIRST");
        });
        ZASSERT(it != own.end());
        return project.commands.command_hash(project.apply_rules(file, *it).command);
    };

    entries("-Wall");
    auto listed = project.list_contexts(file);
    auto item = ranges::find_if(listed, [](const ListedContext& item) {
        return llvm::StringRef(item.label).contains("FIRST");
    });
    ZASSERT(item != listed.end());
    ZASSERT(item->command_hash.has_value());
    ContextChoice choice{Fid{}, std::nullopt, *item->command_hash};
    ZASSERT(project.choice_holds(file, choice));
    auto edited = first();

    entries("-Wextra");
    ZASSERT(first() == edited);
    ZEXPECT(!project.choice_holds(file, choice));
}

ZEST_CASE(LabelsTellFlags) {
    // The macros, optimization levels and standards that set a command
    // apart, clang's or cl's.
    tmp.touch("main.cpp");
    tmp.touch("other.cpp");
    auto entry = [&](llvm::StringRef file, llvm::StringRef flags) {
        return std::format(R"({{"directory": "{}", "file": "{}", "command": "{} {}"}})",
                           json_escape(tmp.root),
                           file,
                           flags,
                           file);
    };
    write_cdb(tmp,
              project.cdb,
              std::format("[{},\n{},\n{},\n{}]",
                          entry("main.cpp", "cl /DFIRST /O1 /std:c++17 /c"),
                          entry("main.cpp", "cl /DSECOND /O2 /std:c++20 /c"),
                          entry("other.cpp", "clang++ -O0 -c"),
                          entry("other.cpp", "clang++ -O2 -c")));
    auto labels = [&](llvm::StringRef file) {
        llvm::SmallVector<std::string> labels;
        for(auto& item: project.list_contexts(fx.intern(tmp.path(file)))) {
            labels.push_back(item.label);
        }
        return labels;
    };
    auto cl = labels("main.cpp");
    ZEXPECT(ranges::contains(cl, "-DFIRST /O1 /std:c++17"));
    ZEXPECT(ranges::contains(cl, "-DSECOND /O2 /std:c++20"));
    auto clang = labels("other.cpp");
    ZEXPECT(ranges::contains(clang, "-O0"));
    ZEXPECT(ranges::contains(clang, "-O2"));
}

ZEST_CASE(ListingRemembered) {
    // The listing holds while the state it reads does: a database loaded
    // without the project following it shows once the commands move.
    tmp.touch("main.cpp");
    auto entry = [&](llvm::StringRef flags) {
        return std::format(R"({{"directory": "{}", "file": "main.cpp", "command": "clang++ {} -c main.cpp"}})",
                           json_escape(tmp.root),
                           flags);
    };
    write_cdb(tmp, project.cdb, std::format("[{}]", entry("-DFIRST")));
    auto file = fx.intern(tmp.path("main.cpp"));
    ZASSERT(project.list_contexts(file).size() == 1u);

    write_cdb(tmp, project.cdb, std::format("[{},\n{}]", entry("-DFIRST"), entry("-DSECOND")));
    ZEXPECT(project.list_contexts(file).size() == 1u);
    project.commands_epoch += 1;
    ZEXPECT(project.list_contexts(file).size() == 2u);
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

    ZASSERT(project.choice_holds(header, server.choice(header)->choice));
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
    project.take_tree_in_process(project.tree_request(host, project.candidates(host).front()));
    ZASSERT(!project.choice_holds(header, server.choice(header)->choice));
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
    ZASSERT(entry->choice.host == host);
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
    saved(project, header, ContextChoice{.base = "outer"});
    saved(inner.project, header, ContextChoice{.base = "inner"});

    server.load_choices(served);
    ZASSERT(server.choice(header)->project == &served);
    server.load_choices(inner);
    auto* entry = server.choice(header);
    ZASSERT(entry->project == &inner);
    ZASSERT(entry->choice.base == "inner");
    ZASSERT(project.choices.dirty);
    ZASSERT(project.saved_choices().empty());
    ZASSERT(!inner.project.choices.dirty);
}

ZEST_CASE(ShallowerChoiceDropped) {
    // The deeper root's choice stands whichever project starts first.
    auto& inner = nested_project();
    auto header = fx.intern(tmp.path("sub/h.h"));
    saved(project, header, ContextChoice{.base = "outer"});
    saved(inner.project, header, ContextChoice{.base = "inner"});

    server.load_choices(inner);
    server.load_choices(served);
    auto* entry = server.choice(header);
    ZASSERT(entry->project == &inner);
    ZASSERT(entry->choice.base == "inner");
    ZASSERT(project.choices.dirty);
    ZASSERT(project.saved_choices().empty());
    ZASSERT(!inner.project.choices.dirty);
}

};  // ZEST_SUITE(Contexts)

}  // namespace
}  // namespace clice::testing
