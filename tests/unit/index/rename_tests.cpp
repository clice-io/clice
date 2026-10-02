#include <algorithm>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "test/merge_unit.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "test/tester.h"
#include "index/query.h"
#include "index/rename.h"
#include "index/symbol_query.h"
#include "vfs/file_system.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"

namespace clice::testing {
namespace {

using index::RenamePlan;

TEST_SUITE(Rename, Tester) {

TempDir dir;
FileTable files;
Project project{files};
index::FreshnessGate gate{project.file_table};
index::IndexQuery query{project.project_index, project.file_table, &gate, nullptr};

Fid main_id;

/// The files the sweep looks at, and those the rename may not edit.
std::vector<std::string> scope;
llvm::StringSet<> foreign;

bool units_pending = false;

/// Per file, the offsets of its nameless marks: the tokens the rename
/// under test is expected to change.
std::map<std::string, std::vector<std::uint32_t>> marked;

std::string file(llvm::StringRef name) {
    return CanonicalPath(Spelling::absolute(dir.path(name))).str();
}

/// Compile the added sources as one unit, merge its index, and put its
/// files on disk with the text the index read.
void merge() {
    ASSERT_TRUE(compile());
    merge_unit(project, *unit, main_id);
    for(auto& entry: sources.all_files) {
        auto path = entry.getKey().str();
        ASSERT_FALSE(static_cast<bool>(vfs::write(path, entry.second.content)));
        marked[path] = entry.second.nameless_offsets;
        if(!llvm::is_contained(scope, path)) {
            scope.push_back(path);
        }
    }
}

/// A workspace file no unit compiles.
void write(llvm::StringRef name, llvm::StringRef content) {
    auto path = file(name);
    ASSERT_FALSE(static_cast<bool>(vfs::write(path, content)));
    scope.push_back(path);
}

RenamePlan plan(const index::RenameTarget& target, llvm::StringRef new_name) {
    auto editable = [&](llvm::StringRef path) {
        return !foreign.contains(path);
    };
    auto read = [&](llvm::StringRef path) -> std::optional<index::SweptText> {
        auto text = vfs::read(path);
        if(!text) {
            return std::nullopt;
        }
        return index::sweep_text((*text)->getBuffer().str(), target.symbol.symbol.name, new_name);
    };
    return index::plan_rename(
        query,
        project.file_table,
        target,
        new_name,
        {.files = scope, .editable = editable, .read = read, .units_pending = units_pending});
}

/// Rename the one symbol the query `name` finds.
std::expected<RenamePlan, std::string> rename(llvm::StringRef name, llvm::StringRef new_name) {
    auto hits = query.locate(*index::SymbolQuery::parse(name));
    if(hits.size() != 1) {
        return std::unexpected(
            std::format("{} symbols match `{}`", hits.size(), std::string_view(name)));
    }
    auto target = index::rename_target(query, hits.front());
    if(!target) {
        return std::unexpected(target.error());
    }
    return plan(*target, new_name);
}

std::expected<index::CursorRename, std::string> cursor(llvm::StringRef at) {
    auto found = query.symbol_at(main_id, point(at));
    if(!found) {
        return std::unexpected("no symbol at the mark");
    }
    return index::rename_at(query, *found);
}

/// Rename the symbol under the main file's mark `at`.
std::expected<RenamePlan, std::string> rename_at(llvm::StringRef at, llvm::StringRef new_name) {
    auto renamed = cursor(at);
    if(!renamed) {
        return std::unexpected(renamed.error());
    }
    return plan(renamed->target, new_name);
}

static std::string listed(llvm::StringRef path, std::uint32_t offset) {
    return std::format("{}@{}\n", llvm::sys::path::filename(path).str(), offset);
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

TEST_CASE(FunctionAcrossUnits) {
    llvm::StringRef header = R"(
        int §compute(int x);
    )";
    add_file(file("util.h"), header);
    add_main(file("a.cpp"), R"(
        #include "util.h"
        int §compute(int x) { return x; }
        // compute in a comment
        const char* text = "compute";
    )");
    merge();
    clear();
    add_file(file("util.h"), header);
    add_main(file("b.cpp"), R"(
        #include "util.h"
        int use() { return §compute(1) + §compute(2); }
    )");
    merge();

    auto renamed = rename("compute", "evaluate");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_TRUE(clean(*renamed));
}

TEST_CASE(ParameterOfOneFunction) {
    add_main(file("a.cpp"), R"(
        int twice(int §value) { int sum = §value + §(cursor)§value; return sum; }
        int once(int value) { return value; }
    )");
    merge();

    auto renamed = rename_at("cursor", "amount");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_TRUE(clean(*renamed));
}

TEST_CASE(ConstructorNamesClass) {
    add_file(file("widget.h"), R"(
        struct §Widget {
            §Widget();
            §Widget(const §Widget&);
            ~§Widget();
        };
    )");
    add_main(file("a.cpp"), R"(
        #include "widget.h"
        §Widget::§(cursor)§Widget() {}
        §Widget::§(dtor)~§Widget() {}
        §Widget make() { §Widget local; return §Widget(local); }
    )");
    merge();

    auto renamed = rename_at("cursor", "Gadget");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_TRUE(clean(*renamed));

    // The token an editor selects is the name, past the destructor's tilde.
    auto destructor = cursor("dtor");
    ASSERT_TRUE(destructor.has_value());
    EXPECT_EQ(destructor->target.symbol.symbol.name, "Widget");
    EXPECT_EQ(destructor->token.range.begin, point("dtor") + 1);
}

TEST_CASE(ClassTemplateFamily) {
    add_main(file("a.cpp"), R"(
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

        §Box<int> boxed(1);
        §Box<double*> pointer;
    )");
    merge();

    auto renamed = rename_at("cursor", "Crate");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_TRUE(clean(*renamed));

    auto from_guide = rename_at("guide", "Crate");
    ASSERT_TRUE(from_guide.has_value());
    EXPECT_EQ(edits(*from_guide), marks());
}

TEST_CASE(FunctionTemplateSpecialization) {
    add_main(file("a.cpp"), R"(
        template <typename T>
        T §(cursor)§pick(T value) { return value; }

        template <>
        int §pick<int>(int value) { return value + 1; }

        int use() { return §pick(1) + §pick<long>(2); }
    )");
    merge();

    auto renamed = rename_at("cursor", "choose");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_TRUE(clean(*renamed));
}

TEST_CASE(VirtualOverrideChain) {
    add_main(file("a.cpp"), R"(
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
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_TRUE(clean(*renamed));
}

TEST_CASE(InternalAcrossUnits) {
    llvm::StringRef header = R"(
        static int §helper() { return 1; }
    )";
    add_file(file("util.h"), header);
    add_main(file("b.cpp"), R"(
        #include "util.h"
        int b() { return §helper(); }
    )");
    merge();
    clear();
    add_file(file("util.h"), header);
    add_main(file("a.cpp"), R"(
        #include "util.h"
        int a() { return §(cursor)§helper(); }
    )");
    merge();

    auto renamed = rename_at("cursor", "assist");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_TRUE(clean(*renamed));
}

TEST_CASE(NamespaceAndQualifiers) {
    add_main(file("a.cpp"), R"(
        namespace §(cursor)§math { int one(); }
        namespace §math { int two(); }
        int §math::one() { return 1; }
        namespace alias = §math;
        using namespace §math;
        int x = §math::two();
    )");
    merge();

    auto renamed = rename_at("cursor", "algebra");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_TRUE(clean(*renamed));
}

TEST_CASE(EnumAndEnumerator) {
    add_main(file("a.cpp"), R"(
        enum class Color { §(cursor)§red, green };
        Color pick() { return Color::§red; }
        int red = 0;
    )");
    merge();

    auto renamed = rename_at("cursor", "crimson");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_TRUE(clean(*renamed));

    clear();
    marked.clear();
    add_main(file("a.cpp"), R"(
        enum class §(cursor)§Color { red, green };
        §Color pick() { return §Color::red; }
    )");
    merge();
    renamed = rename_at("cursor", "Hue");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
}

TEST_CASE(FieldAndAlias) {
    add_main(file("a.cpp"), R"(
        struct Point {
            int §(cursor)§x;
            Point() : §x(0) {}
        };
        int get(Point p) { return p.§x; }
        auto member = &Point::§x;
    )");
    merge();

    auto renamed = rename_at("cursor", "column");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_TRUE(clean(*renamed));

    clear();
    marked.clear();
    add_main(file("a.cpp"), R"(
        using §(cursor)§Size = unsigned;
        §Size count;
        typedef §Size Count;
    )");
    merge();
    renamed = rename_at("cursor", "Length");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
}

TEST_CASE(ConceptAndLabel) {
    add_main(file("a.cpp"), R"(
        template <typename T>
        concept §(cursor)§Small = sizeof(T) < 4;

        template <§Small T>
        void take(T);

        void give(§Small auto value);
        static_assert(§Small<char>);
    )");
    merge();

    auto renamed = rename_at("cursor", "Tiny");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_TRUE(clean(*renamed));

    clear();
    marked.clear();
    add_main(file("a.cpp"), R"(
        void spin() {
        §(cursor)§again:
            goto §again;
        }
    )");
    merge();
    renamed = rename_at("cursor", "retry");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
}

TEST_CASE(RefusedSymbols) {
    add_main(file("a.cpp"), R"(
        #define §(macro)LIMIT 10
        struct Number {
            Number §(plus)operator+(const Number&) const;
            §(conversion)operator int() const;
        };
        int value = LIMIT;
    )");
    merge();

    auto plus = rename_at("plus", "add");
    ASSERT_FALSE(plus.has_value());
    EXPECT_TRUE(plus.error().contains("named by the language"));

    auto conversion = rename_at("conversion", "to_int");
    ASSERT_FALSE(conversion.has_value());
    EXPECT_TRUE(conversion.error().contains("named by the language"));

    auto macro = rename_at("macro", "BOUND");
    ASSERT_FALSE(macro.has_value());
    EXPECT_TRUE(macro.error().contains("macro"));
}

TEST_CASE(InvalidNewNames) {
    add_main(file("a.cpp"), R"(
        int §(cursor)counter = 0;
    )");
    merge();

    for(auto name:
        {"1st", "two words", "", "class", "char8_t", "co_await", "requires", "restrict", "_Bool"}) {
        auto renamed = rename_at("cursor", name);
        ASSERT_TRUE(renamed.has_value());
        EXPECT_EQ(renamed->conflicts.size(), 1U);
        EXPECT_TRUE(renamed->edits.empty());
    }
    for(auto name: {"_Reserved", "two__parts"}) {
        auto renamed = rename_at("cursor", name);
        ASSERT_TRUE(renamed.has_value());
        EXPECT_FALSE(renamed->blocked());
        EXPECT_EQ(renamed->warnings.size(), 1U);
    }

    auto unicode = rename_at("cursor", "café");
    ASSERT_TRUE(unicode.has_value());
    EXPECT_TRUE(unicode->conflicts.empty());

    auto same = rename_at("cursor", "counter");
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(same->edits.empty());
    EXPECT_TRUE(clean(*same));
}

TEST_CASE(SameScopeCollisions) {
    add_main(file("a.cpp"), R"(
        namespace math {
            int §(value)value;
            int total;
            int §(sum)sum(int x);
            int add(double x);
        }
        namespace §(left)left {}
        namespace right {}
        void scope() {
            int §(local)first = 0;
            int second = first;
        }
    )");
    merge();

    auto value = rename_at("value", "total");
    ASSERT_TRUE(value.has_value());
    ASSERT_EQ(value->conflicts.size(), 1U);
    EXPECT_TRUE(value->conflicts.front().contains("already declared in the same scope"));

    auto sum = rename_at("sum", "add");
    ASSERT_TRUE(sum.has_value());
    EXPECT_FALSE(sum->blocked());
    ASSERT_EQ(sum->warnings.size(), 1U);
    EXPECT_TRUE(sum->warnings.front().contains("overloads"));

    auto left = rename_at("left", "right");
    ASSERT_TRUE(left.has_value());
    ASSERT_EQ(left->conflicts.size(), 1U);
    EXPECT_TRUE(left->conflicts.front().contains("merging namespaces"));

    auto local = rename_at("local", "second");
    ASSERT_TRUE(local.has_value());
    EXPECT_FALSE(local->blocked());
    ASSERT_EQ(local->warnings.size(), 1U);
    EXPECT_TRUE(local->warnings.front().contains("local of the same function"));
}

TEST_CASE(MemberHidesInherited) {
    add_main(file("a.cpp"), R"(
        struct Base { int inherited; int §(base)own; };
        struct Derived : Base { int §(derived)extra; int added; };
        struct Sibling : Base { int §(sibling)apart; };
    )");
    merge();

    auto up = rename_at("derived", "inherited");
    ASSERT_TRUE(up.has_value());
    ASSERT_EQ(up->conflicts.size(), 1U);
    EXPECT_TRUE(up->conflicts.front().contains("member of Base"));

    auto down = rename_at("base", "added");
    ASSERT_TRUE(down.has_value());
    ASSERT_EQ(down->conflicts.size(), 1U);
    EXPECT_TRUE(down->conflicts.front().contains("member of Derived"));

    // Siblings never hide each other's members.
    auto across = rename_at("sibling", "extra");
    ASSERT_TRUE(across.has_value());
    EXPECT_TRUE(across->conflicts.empty());
}

TEST_CASE(EnumeratorLookupScope) {
    add_main(file("a.cpp"), R"(
        enum Color { §(red)red };
        enum Mood { blue };
        enum class Tone { §(low)low };
        int high;
    )");
    merge();

    auto unscoped = rename_at("red", "blue");
    ASSERT_TRUE(unscoped.has_value());
    ASSERT_EQ(unscoped->conflicts.size(), 1U);
    EXPECT_TRUE(unscoped->conflicts.front().contains("already declared in the same scope"));

    auto scoped = rename_at("low", "high");
    ASSERT_TRUE(scoped.has_value());
    EXPECT_TRUE(scoped->conflicts.empty());
}

TEST_CASE(MemberTakesClassName) {
    add_main(file("a.cpp"), R"(
        struct Shape { void §(method)draw(); };
        struct §(grid)Grid { int cells; };
    )");
    merge();

    auto method = rename_at("method", "Shape");
    ASSERT_TRUE(method.has_value());
    ASSERT_EQ(method->conflicts.size(), 1U);
    EXPECT_TRUE(method->conflicts.front().contains("names the class the member belongs to"));

    auto grid = rename_at("grid", "cells");
    ASSERT_TRUE(grid.has_value());
    ASSERT_EQ(grid->conflicts.size(), 1U);
    EXPECT_TRUE(grid->conflicts.front().contains("member of the renamed class"));
}

TEST_CASE(ManySameNamedSymbols) {
    std::string source = "int §(cursor)value;\n";
    for(int i = 0; i < 600; i += 1) {
        source += std::format("namespace n{} {{ int target; }}\n", i);
    }
    source += "int target;\n";
    add_main(file("a.cpp"), source);
    merge();

    auto renamed = rename_at("cursor", "target");
    ASSERT_TRUE(renamed.has_value());
    ASSERT_EQ(renamed->conflicts.size(), 1U);
    EXPECT_TRUE(renamed->conflicts.front().contains("already declared in the same scope"));
}

TEST_CASE(MacroNameConflict) {
    add_main(file("a.cpp"), R"(
        #define SHADOW 1
        int §(cursor)value = SHADOW;
    )");
    merge();

    auto renamed = rename_at("cursor", "SHADOW");
    ASSERT_TRUE(renamed.has_value());
    ASSERT_EQ(renamed->conflicts.size(), 1U);
    EXPECT_TRUE(renamed->conflicts.front().contains("is a macro"));
}

TEST_CASE(MacroSpellingsUnconfirmed) {
    add_main(file("a.cpp"), R"(
        int §compute(int x) { return x; }
        #define CALL compute(1)
        int use() { return CALL + §compute(2); }
    )");
    merge();

    auto renamed = rename("compute", "evaluate");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_FALSE(renamed->blocked());
    ASSERT_EQ(renamed->unconfirmed.size(), 2U);
    EXPECT_TRUE(renamed->unconfirmed[0].reason.contains("preprocessor directive"));
    EXPECT_EQ(renamed->unconfirmed[0].line, "#define CALL compute(1)");
    EXPECT_TRUE(renamed->unconfirmed[1].reason.contains("written here as `CALL`"));
}

TEST_CASE(DependentCallHeuristic) {
    add_main(file("a.cpp"), R"(
        int §compute(int x) { return x; }

        template <typename T>
        int apply(T value) { return §compute(value); }
    )");
    merge();

    auto renamed = rename("compute", "evaluate");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    ASSERT_EQ(renamed->edits.size(), 2U);
    EXPECT_FALSE(renamed->edits[0].heuristic);
    EXPECT_TRUE(renamed->edits[1].heuristic);
}

TEST_CASE(ChangedFileBlocks) {
    add_main(file("a.cpp"), R"(
        int compute(int x) { return x; }
        int use() { return compute(1); }
    )");
    merge();
    ASSERT_FALSE(static_cast<bool>(vfs::write(file("a.cpp"), R"(int compute(int x) { return x; }
int later() { return 2; }
)")));

    auto renamed = rename("compute", "evaluate");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_TRUE(renamed->blocked());
    ASSERT_EQ(renamed->stale.size(), 1U);
    EXPECT_TRUE(renamed->stale.front().ends_with("a.cpp"));
}

TEST_CASE(EditedFileMovedOn) {
    // Non-ASCII text is kept with the rows, so no disk read vouches for it.
    add_main(file("a.cpp"), R"(
        // café
        int compute(int x) { return x; }
    )");
    merge();
    ASSERT_FALSE(static_cast<bool>(vfs::write(file("a.cpp"), R"(// café
int renamed(int x) { return x; }
)")));

    auto renamed = rename("compute", "evaluate");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_TRUE(renamed->blocked());
    ASSERT_EQ(renamed->stale.size(), 1U);
    EXPECT_TRUE(renamed->stale.front().ends_with("a.cpp"));
}

TEST_CASE(NewNameInChangedFile) {
    add_main(file("a.cpp"), R"(
        int compute(int x) { return x; }
    )");
    merge();
    clear();
    add_main(file("b.cpp"), R"(
        int other() { return 0; }
    )");
    merge();
    // A declaration of the new name the index has not seen yet.
    ASSERT_FALSE(static_cast<bool>(vfs::write(file("b.cpp"), R"(int other() { return 0; }
int evaluate;
)")));

    auto renamed = rename("compute", "evaluate");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_TRUE(renamed->blocked());
    ASSERT_EQ(renamed->stale.size(), 1U);
    EXPECT_TRUE(renamed->stale.front().ends_with("b.cpp"));
}

TEST_CASE(ExtensionlessHeaderEdited) {
    add_file(file("config"), R"(
        int §compute(int x);
    )");
    add_main(file("a.cpp"), R"(
        #include "config"
        int use() { return §compute(1); }
    )");
    merge();
    std::erase(scope, file("config"));

    auto renamed = rename("compute", "evaluate");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_TRUE(clean(*renamed));
}

TEST_CASE(FilesOutsideTheIndex) {
    add_main(file("a.cpp"), R"(
        int §compute(int x) { return x; }
    )");
    merge();
    write("notes.cpp", "int other() { return compute(3); }\n");

    auto renamed = rename("compute", "evaluate");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(edits(*renamed), marks());
    EXPECT_FALSE(renamed->blocked());
    ASSERT_EQ(renamed->unconfirmed.size(), 1U);
    EXPECT_TRUE(renamed->unconfirmed.front().reason.contains("holds no rows"));

    // A unit of the build the index has yet to reach may include it.
    units_pending = true;
    renamed = rename("compute", "evaluate");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_TRUE(renamed->blocked());
    ASSERT_EQ(renamed->stale.size(), 1U);
    EXPECT_TRUE(renamed->stale.front().ends_with("notes.cpp"));
}

TEST_CASE(EditOutsideWorkspace) {
    dir.mkdir("vendor");
    add_file(file("vendor/lib.h"), R"(
        int compute(int x);
    )");
    add_main(file("a.cpp"), R"(
        #include "vendor/lib.h"
        int use() { return compute(1); }
    )");
    merge();
    std::erase(scope, file("vendor/lib.h"));
    foreign.insert(file("vendor/lib.h"));

    auto renamed = rename("compute", "evaluate");
    ASSERT_TRUE(renamed.has_value());
    ASSERT_EQ(renamed->conflicts.size(), 1U);
    EXPECT_TRUE(renamed->conflicts.front().contains("not a workspace source"));
}

TEST_CASE(ApplyRewritesTokens) {
    add_main(file("a.cpp"), R"(
        int compute(int x) { return x > 0 ? compute(x - 1) : 0; }
    )");
    merge();
    auto renamed = rename("compute", "evaluate");
    ASSERT_TRUE(renamed.has_value());

    auto text = sources.all_files[file("a.cpp")].content;
    auto applied = index::apply_rename(text, *renamed, main_id, "evaluate");
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(*applied, R"(
        int evaluate(int x) { return x > 0 ? evaluate(x - 1) : 0; }
    )");

    // The text moved on: a token no longer spells the old name.
    EXPECT_FALSE(index::apply_rename("int x;", *renamed, main_id, "evaluate").has_value());
    auto longer = text;
    longer.insert(renamed->edits.front().site.range.end, "d");
    EXPECT_FALSE(index::apply_rename(longer, *renamed, main_id, "evaluate").has_value());
}

TEST_CASE(NonAsciiColumns) {
    add_main(file("a.cpp"), R"(
        int compute(int x) { return x; }
        #define NOTE "é" compute
    )");
    merge();

    auto renamed = rename("compute", "evaluate");
    ASSERT_TRUE(renamed.has_value());
    ASSERT_EQ(renamed->unconfirmed.size(), 1U);
    auto& begin = renamed->unconfirmed.front().site.begin;
    EXPECT_EQ(begin.utf16_column + 1, begin.column);
}

TEST_CASE(CursorOffTheName) {
    add_main(file("a.cpp"), R"(
        struct Widget { Widget(int); };
        int compute(int x) { return x; }
        #define CALL compute(1)
        int use() { return §(macro)CALL; }
        Widget made§(paren)(1);
    )");
    merge();

    auto macro = rename_at("macro", "evaluate");
    ASSERT_FALSE(macro.has_value());
    EXPECT_TRUE(macro.error().contains("macro"));

    auto paren = rename_at("paren", "Gadget");
    ASSERT_FALSE(paren.has_value());
    EXPECT_TRUE(paren.error().contains("no name of `Widget`"));
}

};  // TEST_SUITE(Rename)

}  // namespace
}  // namespace clice::testing
