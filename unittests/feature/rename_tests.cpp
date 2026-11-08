module clice;

import :feature.rename;
import :index.query;
import :index.search;
import :index.symbol_query;
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

/// The files the rename may not edit.
llvm::StringSet<> foreign;

bool units_pending = false;

/// Per file, the offsets of its nameless marks: the tokens the rename
/// under test is expected to change.
std::map<std::string, std::vector<std::uint32_t>> marked;

std::string file(llvm::StringRef name) {
    return CanonicalPath(Spelling::absolute(dir.path(name))).str();
}

/// Compile the added sources as one unit, as the background index does,
/// merge its index, and put its files on disk with the text the index
/// read.
void merge() {
    kind = CompilationKind::Whole;
    ZASSERT(compile());
    merge_unit(project, *unit, focus_id);
    for(auto& entry: sources.all_files) {
        auto path = entry.getKey().str();
        ZASSERT(!static_cast<bool>(fs::write_file(path, entry.second.content)));
        marked[path] = entry.second.nameless_offsets;
    }
    // The merge recorded what the unit read as the disk's first look: the
    // writes above change it, as a tick would notice.
    project.file_table.disk.look_all();
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

bool editable(llvm::StringRef path) {
    return !foreign.contains(path);
}

RenamePlan plan(const feature::RenameTarget& target, llvm::StringRef new_name) {
    return feature::plan_rename(query,
                                project.file_table,
                                target,
                                new_name,
                                {.editable = [&](llvm::StringRef path) { return editable(path); },
                                 .read = read,
                                 .units_pending = units_pending});
}

/// The rename target of the one symbol the query `name` finds.
feature::RenameTarget target(llvm::StringRef name) {
    auto hits = index::locate(query, *index::SymbolQuery::parse(name));
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
                                 {.editable = [&](llvm::StringRef path) { return editable(path); },
                                  .read = read,
                                  .units_pending = units_pending});
}

/// The text of `path` with the batch's edits applied.
std::string applied(const feature::BatchPlan& plan, llvm::StringRef path) {
    auto text = vfs::read_text(path);
    ZASSERT(text);
    auto file = project.file_table.find(Spelling::absolute(path));
    ZASSERT(file);
    auto renamed = feature::apply_renames(*text, plan, *file);
    ZASSERT(renamed);
    return std::move(*renamed);
}

/// Rename the one symbol the query `name` finds.
std::expected<RenamePlan, std::string> rename(llvm::StringRef name, llvm::StringRef new_name) {
    auto hits = index::locate(query, *index::SymbolQuery::parse(name));
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

ZEST_CASE(FunctionAcrossUnits) {
    llvm::StringRef header = R"(
        int §compute(int x);
    )";
    add_file(file("util.h"), header);
    add_focus(file("a.cpp"), R"(
        #include "util.h"
        int §compute(int x) { return x; }
        // compute in a comment
        const char* text = "compute";
    )");
    merge();
    clear();
    add_file(file("util.h"), header);
    add_focus(file("b.cpp"), R"(
        #include "util.h"
        int use() { return §compute(1) + §compute(2); }
    )");
    merge();

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(ParameterOfOneFunction) {
    add_focus(file("a.cpp"), R"(
        int twice(int §value) { int sum = §value + §(cursor)§value; return sum; }
        int once(int value) { return value; }
    )");
    merge();

    auto renamed = rename_at("cursor", "amount");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(ConstructorNamesClass) {
    add_file(file("widget.h"), R"(
        struct §Widget {
            §Widget();
            §Widget(const §Widget&);
            ~§Widget();
        };
    )");
    add_focus(file("a.cpp"), R"(
        #include "widget.h"
        §Widget::§(cursor)§Widget() {}
        §Widget::§(dtor)~§Widget() {}
        §Widget make() { §Widget local; return §Widget(local); }
    )");
    merge();

    auto renamed = rename_at("cursor", "Gadget");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));

    // The token an editor selects is the name, past the destructor's tilde.
    auto destructor = cursor("dtor");
    ZASSERT(destructor);
    ZEXPECT(destructor->target.symbol.symbol.name == "Widget");
    ZEXPECT(destructor->token.range.begin == point("dtor") + 1);
}

ZEST_CASE(ClassTemplateFamily) {
    add_focus(file("a.cpp"), R"(
        template <typename T>
        struct §(cursor)§Box {
            §Box(T);
        };

        template <>
        struct §Box<int> {
            §Box(int);
        };

        template <typename T>
        struct §Box<T*> {};

        §(guide)§Box(const char*) -> §Box<int>;

        template <typename T>
        §Box(T*, int) -> §Box<T*>;

        §Box<int> boxed(1);
        §Box<double*> pointer;
    )");
    merge();

    auto renamed = rename_at("cursor", "Crate");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));

    auto from_guide = rename_at("guide", "Crate");
    ZASSERT(from_guide);
    ZEXPECT(edits(*from_guide) == marks());
}

ZEST_CASE(FunctionTemplateSpecialization) {
    add_focus(file("a.cpp"), R"(
        template <typename T>
        T §(cursor)§pick(T value) { return value; }

        template <>
        int §pick<int>(int value) { return value + 1; }

        int use() { return §pick(1) + §pick<long>(2); }
    )");
    merge();

    auto renamed = rename_at("cursor", "choose");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(VirtualOverrideChain) {
    add_focus(file("a.cpp"), R"(
        struct Base { virtual void §run(); };
        struct Derived : Base { void §run() override; };
        struct Other { void run(); };
        void Derived::§run() {}

        void call(Base& base, Derived& derived, Other& other) {
            base.§run();
            derived.§run();
            other.run();
        }
    )");
    merge();

    auto renamed = rename("Base::run", "start");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(InternalAcrossUnits) {
    llvm::StringRef header = R"(
        static int §helper() { return 1; }
    )";
    add_file(file("util.h"), header);
    add_focus(file("b.cpp"), R"(
        #include "util.h"
        int b() { return §helper(); }
    )");
    merge();
    clear();
    add_file(file("util.h"), header);
    add_focus(file("a.cpp"), R"(
        #include "util.h"
        int a() { return §(cursor)§helper(); }
    )");
    merge();

    auto renamed = rename_at("cursor", "assist");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(NamespaceAndQualifiers) {
    add_focus(file("a.cpp"), R"(
        namespace §(cursor)§math { int one(); }
        namespace §math { int two(); }
        int §math::one() { return 1; }
        namespace alias = §math;
        using namespace §math;
        int x = §math::two();
    )");
    merge();

    auto renamed = rename_at("cursor", "algebra");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(EnumAndEnumerator) {
    add_focus(file("a.cpp"), R"(
        enum class Color { §(cursor)§red, green };
        Color pick() { return Color::§red; }
        int red = 0;
    )");
    merge();

    auto renamed = rename_at("cursor", "crimson");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));

    clear();
    marked.clear();
    add_focus(file("a.cpp"), R"(
        enum class §(cursor)§Color { red, green };
        §Color pick() { return §Color::red; }
    )");
    merge();
    renamed = rename_at("cursor", "Hue");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
}

ZEST_CASE(FieldAndAlias) {
    add_focus(file("a.cpp"), R"(
        struct Point {
            int §(cursor)§x;
            Point() : §x(0) {}
        };
        int get(Point p) { return p.§x; }
        auto member = &Point::§x;
    )");
    merge();

    auto renamed = rename_at("cursor", "column");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));

    clear();
    marked.clear();
    add_focus(file("a.cpp"), R"(
        using §(cursor)§Size = unsigned;
        §Size count;
        typedef §Size Count;
    )");
    merge();
    renamed = rename_at("cursor", "Length");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
}

ZEST_CASE(ConceptAndLabel) {
    add_focus(file("a.cpp"), R"(
        template <typename T>
        concept §(cursor)§Small = sizeof(T) < 4;

        template <§Small T>
        void take(T);

        void give(§Small auto value);
        static_assert(§Small<char>);
    )");
    merge();

    auto renamed = rename_at("cursor", "Tiny");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));

    clear();
    marked.clear();
    add_focus(file("a.cpp"), R"(
        void spin() {
        §(cursor)§again:
            goto §again;
        }
    )");
    merge();
    renamed = rename_at("cursor", "retry");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
}

ZEST_CASE(RefusedSymbols) {
    add_focus(file("a.cpp"), R"(
        struct Number {
            Number §(plus)operator+(const Number&) const;
            §(conversion)operator int() const;
        };
    )");
    merge();

    auto plus = rename_at("plus", "add");
    ZASSERT(!plus.has_value());
    ZEXPECT(plus.error().contains("named by the language"));

    auto conversion = rename_at("conversion", "to_int");
    ZASSERT(!conversion.has_value());
    ZEXPECT(conversion.error().contains("named by the language"));
}

ZEST_CASE(InvalidNewNames) {
    add_focus(file("a.cpp"), R"(
        int §(cursor)counter = 0;
    )");
    merge();

    for(auto name:
        {"1st", "two words", "", "class", "char8_t", "co_await", "requires", "restrict", "_Bool"}) {
        auto renamed = rename_at("cursor", name);
        ZASSERT(renamed);
        ZEXPECT(renamed->conflicts.size() == 1U);
        ZEXPECT(renamed->edits.empty());
    }
    for(auto name: {"_Reserved", "two__parts", "_lower"}) {
        auto renamed = rename_at("cursor", name);
        ZASSERT(renamed);
        ZEXPECT(!renamed->blocked());
        ZEXPECT(renamed->warnings.size() == 1U);
    }

    auto unicode = rename_at("cursor", "café");
    ZASSERT(unicode);
    ZEXPECT(unicode->conflicts.empty());

    auto same = rename_at("cursor", "counter");
    ZASSERT(same);
    ZEXPECT(same->edits.empty());
    ZEXPECT(clean(*same));
}

ZEST_CASE(SameScopeCollisions) {
    add_focus(file("a.cpp"), R"(
        namespace math {
            int §(value)value;
            int total;
            int §(sum)sum(int x);
            int add(double x);
        }
        namespace §(left)left {}
        namespace right {}
    )");
    merge();

    auto value = rename_at("value", "total");
    ZASSERT(value);
    ZASSERT(value->conflicts.size() == 1U);
    ZEXPECT(value->conflicts.front().contains("already declared in the same scope"));

    auto sum = rename_at("sum", "add");
    ZASSERT(sum);
    ZEXPECT(!sum->blocked());
    ZASSERT(sum->warnings.size() == 1U);
    ZEXPECT(sum->warnings.front().contains("overloads"));

    auto left = rename_at("left", "right");
    ZASSERT(left);
    ZASSERT(left->conflicts.size() == 1U);
    ZEXPECT(left->conflicts.front().contains("merging namespaces"));
}

ZEST_CASE(ParametersAndLabels) {
    add_focus(file("a.cpp"), R"(
        int sum(int §(declared)first, int second);
        int sum(int §(param)first, int second) { return first + second; }
        int scale(int factor, int amount);
        int scale(int §(apart)value, int amount) { return value * amount; }
        void spin() {
        §(label)again:
        done:
            goto again;
        }
    )");
    merge();

    auto param = rename_at("param", "second");
    ZASSERT(param);
    ZASSERT(param->conflicts.size() == 1U);
    ZEXPECT(param->conflicts.front().contains("parameter of the same function"));

    // Another declaration's parameters are its own.
    auto apart = rename_at("apart", "factor");
    ZASSERT(apart);
    ZEXPECT(clean(*apart));
    auto alike = batch({
        {.target = cursor("declared")->target, .new_name = "total"},
        {.target = cursor("param")->target,    .new_name = "total"},
    });
    ZEXPECT(!alike.blocked());

    auto label = rename_at("label", "done");
    ZASSERT(label);
    ZASSERT(label->conflicts.size() == 1U);
    ZEXPECT(label->conflicts.front().contains("label of the same function"));
}

ZEST_CASE(MemberHidesInherited) {
    add_focus(file("a.cpp"), R"(
        struct Base { int inherited; int §(base)own; };
        struct Derived : Base { int §(derived)extra; int added; };
        struct Sibling : Base { int §(sibling)apart; };
    )");
    merge();

    auto up = rename_at("derived", "inherited");
    ZASSERT(up);
    ZASSERT(up->conflicts.size() == 1U);
    ZEXPECT(up->conflicts.front().contains("member of Base"));

    auto down = rename_at("base", "added");
    ZASSERT(down);
    ZASSERT(down->conflicts.size() == 1U);
    ZEXPECT(down->conflicts.front().contains("member of Derived"));

    // Siblings never hide each other's members.
    auto across = rename_at("sibling", "extra");
    ZASSERT(across);
    ZEXPECT(across->conflicts.empty());
}

ZEST_CASE(EnumeratorLookupScope) {
    add_focus(file("a.cpp"), R"(
        enum Color { §(red)red };
        enum Mood { blue };
        enum class Tone { §(low)low };
        int high;
    )");
    merge();

    auto unscoped = rename_at("red", "blue");
    ZASSERT(unscoped);
    ZASSERT(unscoped->conflicts.size() == 1U);
    ZEXPECT(unscoped->conflicts.front().contains("already declared in the same scope"));

    auto scoped = rename_at("low", "high");
    ZASSERT(scoped);
    ZEXPECT(scoped->conflicts.empty());
}

ZEST_CASE(MemberTakesClassName) {
    add_focus(file("a.cpp"), R"(
        struct Shape { void §(method)draw(); };
        struct §(grid)Grid { int cells; };
    )");
    merge();

    auto method = rename_at("method", "Shape");
    ZASSERT(method);
    ZASSERT(method->conflicts.size() == 1U);
    ZEXPECT(method->conflicts.front().contains("names the class the member belongs to"));

    auto grid = rename_at("grid", "cells");
    ZASSERT(grid);
    ZASSERT(grid->conflicts.size() == 1U);
    ZEXPECT(grid->conflicts.front().contains("member of the renamed class"));
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

ZEST_CASE(MacroNameConflict) {
    add_focus(file("a.cpp"), R"(
        #define SHADOW 1
        int §(cursor)value = SHADOW;
    )");
    merge();

    auto renamed = rename_at("cursor", "SHADOW");
    ZASSERT(renamed);
    ZASSERT(renamed->conflicts.size() == 1U);
    ZEXPECT(renamed->conflicts.front().contains("is a macro"));
}

ZEST_CASE(MacroSpellingsUnconfirmed) {
    add_focus(file("a.cpp"), R"(
        int §compute(int x) { return x; }
        #define CALL compute(1)
        int use() { return CALL + §compute(2); }
    )");
    merge();

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(!renamed->blocked());
    ZASSERT(renamed->unconfirmed.size() == 2U);
    ZEXPECT(renamed->unconfirmed[0].reason.contains("#define body"));
    ZEXPECT(renamed->unconfirmed[0].line == "#define CALL compute(1)");
    ZEXPECT(renamed->unconfirmed[1].reason.contains("written here as `CALL`"));
}

ZEST_CASE(DependentCallCandidate) {
    // The overloads a dependent call finds where it is written are its
    // for certain, instantiated or not.
    add_focus(file("a.cpp"), R"(
        int §compute(int x) { return x; }

        template <typename T>
        int apply(T value) { return §compute(value); }
    )");
    merge();

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(renamed->unconfirmed.empty());
}

ZEST_CASE(InstantiationsDecideDependentNames) {
    add_focus(file("a.cpp"), R"(
        struct A { void §(cursor)go(); };
        struct B { void go(); };

        template <typename T>
        void one(T t) { t.§(one)go(); }

        template <typename T>
        void both(T t) { t.§(both)go(); }

        template <typename T>
        void none(T t) { t.§(none)go(); }

        void use() {
            one(A{});
            both(A{});
            both(B{});
        }
    )");
    merge();

    // `both` calls B::go too: its name can neither change nor stay.
    auto alone = rename_at("cursor", "start");
    ZASSERT(alone);
    ZASSERT(alone->conflicts.size() == 1U);
    ZEXPECT(alone->conflicts.front().contains("`B::go`"));

    auto together = batch({
        {.target = target("A::go"), .new_name = "start"},
        {.target = target("B::go"), .new_name = "start"},
    });
    ZEXPECT(!together.blocked());
    ZEXPECT(together.edits.size() == 4U);

    // From the template, the name is what its instantiations resolve it to.
    auto from_one = cursor("one");
    ZASSERT(from_one);
    ZEXPECT(from_one->target.symbol.symbol.hash == target("A::go").symbol.symbol.hash);
    auto from_both = cursor("both");
    ZASSERT(!from_both);
    ZEXPECT(from_both.error().contains("`B::go`"));
    ZEXPECT(!cursor("none"));
}

ZEST_CASE(InstantiationsOfOneRename) {
    // Overrides are one rename: instantiations naming both agree.
    add_focus(file("a.cpp"), R"(
        struct Base { virtual void §go(); };
        struct Derived : Base { void §go() override; };

        template <typename T>
        void run(T t) { t.§(call)§go(); }

        void use() {
            run(Base{});
            run(Derived{});
        }
    )");
    merge();

    auto renamed = rename_at("call", "start");
    ZASSERT(renamed);
    ZEXPECT(!renamed->blocked());
    ZEXPECT(edits(*renamed) == marks());
}

ZEST_CASE(GuessesNeedAnInstantiation) {
    add_focus(file("a.cpp"), R"(
        template <typename T>
        struct Box { static constexpr int §(cursor)§value = 1; };

        template <typename T>
        int read() { return Box<T>::§(read)§value; }

        template <typename T>
        int peek() { return Box<T>::§(peek)value; }

        int use() { return read<int>(); }
    )");
    merge();

    auto renamed = rename_at("cursor", "amount");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZASSERT(renamed->unconfirmed.size() == 1U);
    ZEXPECT(renamed->unconfirmed.front().reason.contains("no instantiation"));
    ZEXPECT(renamed->unconfirmed.front().line.contains("peek"));

    // Renamed from the guessed name, the instantiation decides.
    auto from_read = cursor("read");
    ZASSERT(from_read);
    ZEXPECT(from_read->target.symbol.symbol.name == "value");
    auto from_peek = cursor("peek");
    ZASSERT(!from_peek);
    ZEXPECT(from_peek.error().contains("no instantiation"));
}

ZEST_CASE(DependentCallOverloads) {
    add_focus(file("a.cpp"), R"(
        int §(cursor)§compute(int x) { return x; }
        double compute(double x) { return x; }

        template <typename T>
        T apply(T value) { return compute(value); }
    )");
    merge();

    auto renamed = rename_at("cursor", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZASSERT(renamed->unconfirmed.size() == 1U);
    ZEXPECT(renamed->unconfirmed.front().reason.contains("also refers to"));
}

ZEST_CASE(ChangedFileBlocks) {
    add_focus(file("a.cpp"), R"(
        int compute(int x) { return x; }
        int use() { return compute(1); }
    )");
    merge();
    ZASSERT(!static_cast<bool>(fs::write_file(file("a.cpp"), R"(int compute(int x) { return x; }
int later() { return 2; }
)")));

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(renamed->blocked());
    ZASSERT(renamed->stale.size() == 1U);
    ZEXPECT(renamed->stale.front().ends_with("a.cpp"));
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
    clear();
    add_focus(file("b.cpp"), R"(
        int other() { return 0; }
    )");
    merge();
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

ZEST_CASE(ExtensionlessHeaderEdited) {
    add_file(file("config"), R"(
        int §compute(int x);
    )");
    add_focus(file("a.cpp"), R"(
        #include "config"
        int use() { return §compute(1); }
    )");
    merge();

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(UnitsPendingWarn) {
    add_focus(file("a.cpp"), R"(
        int §compute(int x) { return x; }
    )");
    merge();
    units_pending = true;

    // A unit the index has yet to reach may name it too.
    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(!renamed->blocked());
    ZASSERT(renamed->warnings.size() == 1U);
    ZEXPECT(renamed->warnings.front().contains("not indexed yet"));
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

ZEST_CASE(EditOutsideWorkspace) {
    dir.mkdir("vendor");
    add_file(file("vendor/lib.h"), R"(
        int compute(int x);
    )");
    add_focus(file("a.cpp"), R"(
        #include "vendor/lib.h"
        int use() { return compute(1); }
    )");
    merge();
    foreign.insert(file("vendor/lib.h"));

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZASSERT(renamed->conflicts.size() == 1U);
    ZEXPECT(renamed->conflicts.front().contains("not a source of the project"));
}

ZEST_CASE(MacroAcrossUnits) {
    llvm::StringRef header = R"(
        #ifndef §LIMIT
        #define §LIMIT 10
        #endif
    )";
    add_file(file("limits.h"), header);
    add_focus(file("a.cpp"), R"(
        #include "limits.h"
        int a = §LIMIT;
        #ifdef §LIMIT
        int b = §LIMIT + 1;
        #endif
    )");
    merge();
    clear();
    add_file(file("limits.h"), header);
    add_focus(file("b.cpp"), R"(
        #include "limits.h"
        #if defined(§LIMIT) && §LIMIT > 5
        int c = §(cursor)§LIMIT;
        #endif
        #undef §LIMIT
    )");
    merge();
    clear();
    // A file neither defining nor using it tests the name for its own.
    add_focus(file("c.cpp"), R"(
        #ifdef LIMIT
        #endif
    )");
    merge();

    auto renamed = rename("LIMIT", "BOUND");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(!renamed->blocked());
    ZASSERT(renamed->unconfirmed.size() == 1U);
    ZEXPECT(renamed->unconfirmed.front().site.path.ends_with("c.cpp"));
    ZEXPECT(renamed->unconfirmed.front().reason.contains("no #define of it reaches"));
}

ZEST_CASE(MacroBranches) {
    add_focus(file("a.cpp"), R"(
        #undef §LIMIT
        #ifdef BIG
        #define §LIMIT 100
        #elif defined(§LIMIT)
        #endif
        #ifndef BIG
        #define §LIMIT 10
        #endif
        int a = §(cursor)§LIMIT;
        #undef §LIMIT
        #define §LIMIT 5
        int b = §LIMIT;
    )");
    merge();

    auto renamed = rename_at("cursor", "BOUND");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(MacroArguments) {
    add_focus(file("a.cpp"), R"(
        #define §F() 1
        #define APPLY(f) f()
        #define TWICE(f) f() + f()
        int a = APPLY(§F);
        int b = TWICE(§F);
        int c = §(cursor)§F();
    )");
    merge();

    auto renamed = rename_at("cursor", "G");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(MacroInOtherBodies) {
    add_focus(file("a.cpp"), R"(
        #define §LIMIT 10
        #define WRAP (§LIMIT + 1)
        #define UNUSED (LIMIT * 2)
        int a = WRAP;
        int b = §(cursor)§LIMIT;
    )");
    merge();

    // A body no expansion runs names nothing the index knows.
    auto renamed = rename_at("cursor", "BOUND");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(MacroTestedInBody) {
    add_focus(file("a.cpp"), R"(
        #define HAS_LIMIT defined(LIMIT)
        #if HAS_LIMIT
        #endif
        #define §(cursor)§LIMIT 10
        int a = §LIMIT;
    )");
    merge();

    auto renamed = rename_at("cursor", "BOUND");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(MacroPastedUse) {
    add_focus(file("a.cpp"), R"(
        #define CAT(a, b) a##b
        #define §(cursor)§LIMIT 10
        int a = CAT(LIM, IT);
    )");
    merge();

    auto renamed = rename_at("cursor", "BOUND");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(!renamed->blocked());
    ZASSERT(renamed->unconfirmed.size() == 1U);
    ZEXPECT(renamed->unconfirmed.front().reason.contains("written here as `CAT`"));
}

ZEST_CASE(MacroUnrelatedDefinitions) {
    add_file(file("a.h"), R"(
        #define CHECK 1
    )");
    add_focus(file("a.cpp"), R"(
        #include "a.h"
        int a = CHECK;
    )");
    merge();
    clear();
    add_file(file("b.h"), R"(
        #define §CHECK 2
    )");
    add_focus(file("b.cpp"), R"(
        #include "b.h"
        int b = §(cursor)§CHECK;
    )");
    merge();

    auto renamed = rename_at("cursor", "VERIFY");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(MacroLinkedThroughBody) {
    llvm::StringRef header = R"(
        int use(const char* name);
        #define LOG() use(§DEBUG_TYPE)
    )";
    add_file(file("log.h"), header);
    add_focus(file("a.cpp"), R"(
        #include "log.h"
        #define §DEBUG_TYPE "a"
        int a = LOG();
        #undef §DEBUG_TYPE
    )");
    merge();
    clear();
    add_file(file("log.h"), header);
    add_focus(file("b.cpp"), R"(
        #include "log.h"
        #define §(cursor)§DEBUG_TYPE "b"
        int b = LOG();
    )");
    merge();

    // Each file's definition is what the one body names in it.
    auto renamed = rename_at("cursor", "LOG_TYPE");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(ApplyRewritesTokens) {
    add_focus(file("a.cpp"), R"(
        int compute(int x) { return x > 0 ? compute(x - 1) : 0; }
    )");
    merge();
    auto renamed = batch({
        {.target = target("compute"), .new_name = "evaluate"}
    });
    ZASSERT(!renamed.blocked());
    ZEXPECT(applied(renamed, file("a.cpp")) == R"(
        int evaluate(int x) { return x > 0 ? evaluate(x - 1) : 0; }
    )");

    // The text moved on: a token no longer spells the old name.
    ZEXPECT(!feature::apply_renames("int x;", renamed, focus_id).has_value());
    auto longer = sources.all_files[file("a.cpp")].content;
    longer.insert(renamed.edits.front().edit.site.range.end, "d");
    ZEXPECT(!feature::apply_renames(longer, renamed, focus_id).has_value());
}

ZEST_CASE(DollarNames) {
    add_focus(file("a.cpp"), R"(
        int §my$count = 0;
        int use() { return §my$count; }
    )");
    merge();
    auto renamed = rename("my$count", "total");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(BatchSwapsNames) {
    add_focus(file("a.cpp"), R"(
        int alpha;
        int beta;
        int gamma() { return alpha + beta; }
    )");
    merge();
    auto renamed = batch({
        {.target = target("alpha"), .new_name = "beta" },
        {.target = target("beta"),  .new_name = "alpha"},
        {.target = target("gamma"), .new_name = "delta"},
    });
    ZEXPECT(!renamed.blocked());
    for(auto& plan: renamed.plans) {
        ZEXPECT(plan.warnings.empty());
        ZEXPECT(plan.unconfirmed.empty());
    }
    ZEXPECT(applied(renamed, file("a.cpp")) == R"(
        int beta;
        int alpha;
        int delta() { return beta + alpha; }
    )");
}

ZEST_CASE(BatchJudgesFinalNames) {
    add_focus(file("a.cpp"), R"(
        int first;
        int second;
        int taken;
        struct Row { int entry; };
        void shrink(int);
        void grow(double);
    )");
    merge();

    // Two symbols of one scope given one name collide; a name the batch
    // frees is free.
    auto collide = batch({
        {.target = target("first"),  .new_name = "same"},
        {.target = target("second"), .new_name = "same"},
    });
    ZASSERT(collide.plans.size() == 2U);
    ZASSERT(collide.plans[0].conflicts.size() == 1U);
    ZEXPECT(collide.plans[0].conflicts.front().contains("already declared in the same scope"));
    ZASSERT(collide.plans[1].conflicts.size() == 1U);
    auto freed = batch({
        {.target = target("first"), .new_name = "taken"},
        {.target = target("taken"), .new_name = "kept" },
    });
    ZEXPECT(!freed.blocked());

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

ZEST_CASE(BatchSharedTokens) {
    add_focus(file("a.cpp"), R"(
        int §(int)scale(int x) { return x; }
        double §(double)scale(double x) { return x; }
        template <typename T> T twice(T v) { return scale(v) * 2; }
    )");
    merge();
    auto overload = [&](llvm::StringRef mark) {
        auto at = cursor(mark);
        ZASSERT(at);
        return std::move(at->target);
    };

    // Overloads renamed alike stay overloads, and the call a template
    // leaves open between them is theirs.
    auto alike = batch({
        {.target = overload("int"),    .new_name = "resize"},
        {.target = overload("double"), .new_name = "resize"},
    });
    ZEXPECT(!alike.blocked());
    ZEXPECT(alike.edits.size() == 3U);
    for(auto& plan: alike.plans) {
        ZEXPECT(plan.warnings.empty());
        ZEXPECT(plan.unconfirmed.empty());
    }

    auto apart = batch({
        {.target = overload("int"),    .new_name = "resize" },
        {.target = overload("double"), .new_name = "rescale"},
    });
    ZASSERT(apart.plans[0].conflicts.size() == 1U);
    ZEXPECT(apart.plans[0].conflicts.front().contains("another rename makes `rescale`"));
}

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

ZEST_CASE(CursorOffTheName) {
    add_focus(file("a.cpp"), R"(
        struct Widget { Widget(int); };
        int compute(int x) { return x; }
        #define CALL compute(1)
        int use() { return §(macro)CALL; }
        Widget made§(paren)(1);
    )");
    merge();

    // An invocation names the macro, not what its body calls.
    auto macro = cursor("macro");
    ZASSERT(macro);
    ZEXPECT(macro->target.symbol.symbol.name == "CALL");

    auto paren = rename_at("paren", "Gadget");
    ZASSERT(!paren.has_value());
    ZEXPECT(paren.error().contains("no name of `Widget`"));
}

};  // ZEST_SUITE(Rename)

}  // namespace
}  // namespace clice::testing
