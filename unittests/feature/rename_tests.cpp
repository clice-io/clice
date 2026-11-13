module clice;

import :feature.rename;
import :index.query;
import :index.query.locate;
import :index.search.locator;
import :unittests.utils.merge_unit;
import :unittests.utils.temp_dir;
import :unittests.utils.tester;
import :unittests.utils;
import :vfs.disk_state;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;

namespace clice::testing {
namespace {

using feature::RenamePlan;

ZEST_SUITE(Rename, Tester) {

TempDir dir;
FileTable files;
Project project{files};
index::FreshnessGate gate{project.file_table};
index::IndexQuery query{project.index, project.file_table, &gate};

Fid focus_id;

/// Per file, the offsets of its nameless marks: the tokens the rename
/// under test is expected to change.
std::map<std::string, std::vector<std::uint32_t>> marked;

std::string file(llvm::StringRef name) {
    return CanonicalPath(Spelling::absolute(dir.path(name))).str();
}

/// Compile the sources added to `tester` as one unit, as the background
/// index does, merge its index, and put its files on disk with the text the
/// index read.
void merge(Tester& tester) {
    tester.kind = CompilationKind::Whole;
    ZASSERT(tester.compile());
    merge_unit(project, *tester.unit, focus_id);
    for(auto& entry: tester.sources.all_files) {
        auto path = entry.getKey().str();
        ZASSERT(!static_cast<bool>(fs::write_file(path, entry.second.content)));
        marked[path] = entry.second.nameless_offsets;
    }
    // The merge recorded what the unit read as the disk's first look: the
    // writes above change it, as a tick would notice.
    project.file_table.disk.look_all();
}

void merge() {
    merge(*this);
}

/// Change a merged file on disk after its unit was indexed, as an editor
/// saving it does; a tick of the disk watch notices.
void change(llvm::StringRef name, llvm::StringRef content) {
    ZASSERT(!static_cast<bool>(fs::write_file(file(name), content)));
    project.file_table.disk.look_all();
}

static std::optional<std::string> read(llvm::StringRef path) {
    auto text = vfs::read_text(path);
    return text ? std::optional(std::move(*text)) : std::nullopt;
}

static bool editable(llvm::StringRef) {
    return true;
}

RenamePlan plan(const feature::RenameTarget& target, llvm::StringRef new_name) {
    return feature::plan_rename(query,
                                project.file_table,
                                target,
                                new_name,
                                {.editable = editable, .read = read});
}

/// The rename target of the one symbol the query `name` finds.
feature::RenameTarget target(llvm::StringRef name) {
    auto hits = index::locate(query, *index::Locator::parse(name));
    ZASSERT(hits.size() == 1U);
    auto found = feature::rename_target(query, hits.front());
    ZASSERT(found);
    return std::move(*found);
}

/// Plan the renames together, as a batch.
feature::BatchPlan batch(std::vector<feature::Renaming> renamings) {
    return feature::plan_renames(query,
                                 project.file_table,
                                 renamings,
                                 {.editable = editable, .read = read});
}

/// Rename the one symbol the query `name` finds.
std::expected<RenamePlan, std::string> rename(llvm::StringRef name, llvm::StringRef new_name) {
    auto hits = index::locate(query, *index::Locator::parse(name));
    if(hits.size() != 1) {
        return std::unexpected(
            std::format("{} symbols match `{}`", hits.size(), std::string_view(name)));
    }
    auto target = feature::rename_target(query, hits.front());
    if(!target) {
        return std::unexpected(target.error());
    }
    return plan(*target, new_name);
}

std::expected<feature::CursorRename, std::string> cursor(llvm::StringRef at) {
    auto found = query.symbol_at(focus_id, point(at));
    if(!found) {
        return std::unexpected("no symbol at the mark");
    }
    return feature::rename_at(query, *found);
}

/// Rename the symbol under the focus file's mark `at`.
std::expected<RenamePlan, std::string> rename_at(llvm::StringRef at, llvm::StringRef new_name) {
    auto renamed = cursor(at);
    if(!renamed) {
        return std::unexpected(renamed.error());
    }
    return plan(renamed->target, new_name);
}

static std::string listed(llvm::StringRef path, std::uint32_t offset) {
    return std::format("{}@{}\n", path::filename(path), offset);
}

/// The plan's edits, a `file@offset` line each.
std::string edits(const RenamePlan& plan) {
    std::string out;
    for(auto& edit: plan.edits) {
        out += listed(edit.site.path, edit.site.range.begin);
    }
    return out;
}

/// The marks of every merged file, as edits() lists the plan's.
std::string marks() {
    std::string out;
    for(auto& [path, offsets]: marked) {
        for(auto offset: offsets) {
            out += listed(path, offset);
        }
    }
    return out;
}

bool clean(const RenamePlan& plan) {
    return !plan.blocked() && plan.unconfirmed.empty() && plan.warnings.empty();
}

ZEST_CASE(ManySameNamedSymbols) {
    std::string source = "int §(cursor)value;\n";
    for(int i = 0; i < 600; i += 1) {
        source += std::format("namespace n{} {{ int target; }}\n", i);
    }
    source += "int target;\n";
    add_focus(file("a.cpp"), source);
    merge();

    auto renamed = rename_at("cursor", "target");
    ZASSERT(renamed);
    ZASSERT(renamed->conflicts.size() == 1U);
    ZEXPECT(renamed->conflicts.front().contains("already declared in the same scope"));
}

ZEST_CASE(EditedFileMovedOn) {
    // Non-ASCII text is kept with the index, so no disk read confirms it.
    add_focus(file("a.cpp"), R"(
        // café
        int compute(int x) { return x; }
    )");
    merge();
    ZASSERT(!static_cast<bool>(fs::write_file(file("a.cpp"), R"(// café
int renamed(int x) { return x; }
)")));

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(renamed->blocked());
    ZASSERT(renamed->stale.size() == 1U);
    ZEXPECT(renamed->stale.front().ends_with("a.cpp"));
}

ZEST_CASE(NewNameInChangedFile) {
    add_focus(file("a.cpp"), R"(
        int compute(int x) { return x; }
    )");
    merge();
    Tester other;
    other.add_focus(file("b.cpp"), R"(
        int other() { return 0; }
    )");
    merge(other);
    // A declaration of the new name the index has not seen yet.
    change("b.cpp", R"(int other() { return 0; }
int evaluate;
)");

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(renamed->blocked());
    ZASSERT(renamed->stale.size() == 1U);
    ZEXPECT(renamed->stale.front().ends_with("b.cpp"));
}

ZEST_CASE(LocalsStayHome) {
    add_file(file("b.h"), R"(
        #define ROWS rows
    )");
    add_file(file("c.h"), R"(
        int columns;
    )");
    add_focus(file("a.cpp"), R"(
        #include "b.h"
        #include "c.h"
        int count(int §(cursor)§rows) { return §rows; }
    )");
    merge();

    // Another file changed since it was indexed reaches a parameter only
    // through a macro it defines.
    change("c.h", "int columns = rows;\n");
    auto renamed = rename_at("cursor", "entries");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));

    change("b.h", "#define entries 1\n");
    renamed = rename_at("cursor", "entries");
    ZASSERT(renamed);
    ZEXPECT(renamed->blocked());
    ZASSERT(renamed->stale.size() == 1U);
    ZEXPECT(renamed->stale.front().ends_with("b.h"));
}

ZEST_CASE(BatchJudgesFinalNames) {
    add_focus(file("a.cpp"), R"(
        struct Row { int entry; };
        void shrink(int);
        void grow(double);
    )");
    merge();

    // A member named as its class will be.
    auto member = batch({
        {.target = target("Row"),        .new_name = "Entry"},
        {.target = target("Row::entry"), .new_name = "Entry"},
    });
    ZASSERT(member.plans[1].conflicts.size() == 1U);
    ZEXPECT(member.plans[1].conflicts.front().contains("names the class the member belongs to"));
    auto vacated = batch({
        {.target = target("Row"),        .new_name = "Table"},
        {.target = target("Row::entry"), .new_name = "Row"  },
    });
    ZEXPECT(!vacated.blocked());

    // Two functions given one name overload each other.
    auto overloaded = batch({
        {.target = target("shrink"), .new_name = "resize"},
        {.target = target("grow"),   .new_name = "resize"},
    });
    ZEXPECT(!overloaded.blocked());
    ZASSERT(overloaded.plans[0].warnings.size() == 1U);
    ZEXPECT(overloaded.plans[0].warnings.front().contains("already names a function"));
}

ZEST_CASE(BatchRenamesOnce) {
    add_focus(file("a.cpp"), R"(
        struct Base { virtual void run(); };
        struct Derived : Base { void run() override; };
    )");
    merge();
    auto twice = batch({
        {.target = target("Base::run"),    .new_name = "start"},
        {.target = target("Derived::run"), .new_name = "start"},
    });
    ZEXPECT(!twice.blocked());
    ZEXPECT(twice.plans[1].edits.empty());
    ZEXPECT(twice.plans[1].warnings.size() == 1U);

    auto apart = batch({
        {.target = target("Base::run"),    .new_name = "start" },
        {.target = target("Derived::run"), .new_name = "launch"},
    });
    ZASSERT(apart.plans[1].conflicts.size() == 1U);
    ZEXPECT(apart.plans[1].conflicts.front().contains("as well"));
}

ZEST_CASE(BatchParametersApart) {
    add_focus(file("a.cpp"), R"(
        int sum(int §(declared)first, int second);
        int sum(int §(defined)first, int second) { return first + second; }
    )");
    merge();

    // Each declaration's parameter is a rename of its own.
    auto alike = batch({
        {.target = cursor("declared")->target, .new_name = "total"},
        {.target = cursor("defined")->target,  .new_name = "total"},
    });
    ZEXPECT(!alike.blocked());
}

ZEST_CASE(BatchSharedDependentCall) {
    add_focus(file("a.cpp"), R"(
        struct A { void go(); };
        struct B { void go(); };

        template <typename T>
        void one(T t) { t.go(); }

        template <typename T>
        void both(T t) { t.go(); }

        void use() {
            one(A{});
            both(A{});
            both(B{});
        }
    )");
    merge();

    // Renamed together, the name `both` calls changes with them.
    auto together = batch({
        {.target = target("A::go"), .new_name = "start"},
        {.target = target("B::go"), .new_name = "start"},
    });
    ZEXPECT(!together.blocked());
    ZEXPECT(together.edits.size() == 4U);
}

/// The text moving on between a plan and its writing is a race no case
/// stages.
ZEST_CASE(ApplyRefusesMovedText) {
    add_focus(file("a.cpp"), R"(
        int compute(int x) { return x > 0 ? compute(x - 1) : 0; }
    )");
    merge();
    auto renamed = batch({
        {.target = target("compute"), .new_name = "evaluate"}
    });
    ZASSERT(!renamed.blocked());
    auto text = sources.all_files[file("a.cpp")].content;
    ZEXPECT(feature::apply_renames(text, renamed, focus_id).has_value());

    // A token no longer spells the old name.
    ZEXPECT(!feature::apply_renames("int x;", renamed, focus_id).has_value());
    text.insert(renamed.edits.front().edit.site.range.end, "d");
    ZEXPECT(!feature::apply_renames(text, renamed, focus_id).has_value());
}

/// The unconfirmed sites' UTF-16 columns reach no rendered answer: the
/// editor's notice names lines, the command's output byte columns.
ZEST_CASE(NonAsciiColumns) {
    add_focus(file("a.cpp"), R"(
        int compute(int x) { return x; }
        #define NOTE(x) ("é", compute(x))
        int use() { return NOTE(1); }
    )");
    merge();

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZASSERT(renamed->unconfirmed.size() == 2U);
    auto& begin = renamed->unconfirmed.front().site.begin;
    ZEXPECT(begin.utf16_column + 1 == begin.column);
}

};  // ZEST_SUITE(Rename)

}  // namespace
}  // namespace clice::testing
