module;

#include "modules/prelude.h"

module clice;

import :index.query;
import :index.rename;
import :index.symbol_query;
import :unittests.utils.merge_unit;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :unittests.utils.tester;
import :vfs.file_system;

namespace clice::testing {
namespace {

using index::RenamePlan;

ZEST_SUITE(Rename, Tester) {

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
    ZASSERT(compile());
    merge_unit(project, *unit, main_id);
    for(auto& entry: sources.all_files) {
        auto path = entry.getKey().str();
        ZASSERT(!static_cast<bool>(vfs::write(path, entry.second.content)));
        marked[path] = entry.second.nameless_offsets;
        if(!llvm::is_contained(scope, path)) {
            scope.push_back(path);
        }
    }
}

/// A workspace file no unit compiles.
void write(llvm::StringRef name, llvm::StringRef content) {
    auto path = file(name);
    ZASSERT(!static_cast<bool>(vfs::write(path, content)));
    scope.push_back(path);
}

/// The disk's files swept for the names of the renames planned, each
/// read once.
llvm::StringSet<> names;
llvm::StringMap<std::optional<index::SweptText>> swept;

const index::SweptText* read(llvm::StringRef path) {
    auto [it, inserted] = swept.try_emplace(path);
    if(inserted) {
        if(auto text = vfs::read(path)) {
            it->second = index::sweep_text((*text)->getBuffer().str(), names);
        }
    }
    return it->second ? &*it->second : nullptr;
}

bool editable(llvm::StringRef path) {
    return !foreign.contains(path);
}

RenamePlan plan(const index::RenameTarget& target, llvm::StringRef new_name) {
    names = {target.symbol.symbol.name, new_name};
    swept.clear();
    return index::plan_rename(query,
                              project.file_table,
                              target,
                              new_name,
                              {.files = scope,
                               .editable = [&](llvm::StringRef path) { return editable(path); },
                               .read = [&](llvm::StringRef path) { return read(path); },
                               .units_pending = units_pending});
}

/// The rename target of the one symbol the query `name` finds.
index::RenameTarget target(llvm::StringRef name) {
    auto hits = query.locate(*index::SymbolQuery::parse(name));
    ZASSERT(hits.size() == 1U);
    auto found = index::rename_target(query, hits.front());
    ZASSERT(found);
    return std::move(*found);
}

/// Plan the renames together, as a batch.
index::BatchPlan batch(std::vector<index::Renaming> renamings) {
    names.clear();
    swept.clear();
    for(auto& [target, new_name]: renamings) {
        names.insert(target.symbol.symbol.name);
        names.insert(new_name);
    }
    return index::plan_renames(query,
                               project.file_table,
                               renamings,
                               {.files = scope,
                                .editable = [&](llvm::StringRef path) { return editable(path); },
                                .read = [&](llvm::StringRef path) { return read(path); },
                                .units_pending = units_pending});
}

/// The text of `path` with the batch's edits applied.
std::string applied(const index::BatchPlan& plan, llvm::StringRef path) {
    auto text = vfs::read(path);
    ZASSERT(text);
    auto file = project.file_table.find(Spelling::absolute(path));
    ZASSERT(file);
    auto renamed = index::apply_renames((*text)->getBuffer(), plan, *file);
    ZASSERT(renamed);
    return std::move(*renamed);
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

ZEST_CASE(FunctionAcrossUnits) {
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
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(ParameterOfOneFunction) {
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
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
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));

    auto from_guide = rename_at("guide", "Crate");
    ZASSERT(from_guide);
    ZEXPECT(edits(*from_guide) == marks());
}

ZEST_CASE(FunctionTemplateSpecialization) {
    add_main(file("a.cpp"), R"(
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
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(InternalAcrossUnits) {
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
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(NamespaceAndQualifiers) {
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
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(EnumAndEnumerator) {
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
        enum class §(cursor)§Color { red, green };
        §Color pick() { return §Color::red; }
    )");
    merge();
    renamed = rename_at("cursor", "Hue");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
}

ZEST_CASE(FieldAndAlias) {
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
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));

    clear();
    marked.clear();
    add_main(file("a.cpp"), R"(
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
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));

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
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
}

ZEST_CASE(RefusedSymbols) {
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
    ZASSERT(!plus.has_value());
    ZEXPECT(plus.error().contains("named by the language"));

    auto conversion = rename_at("conversion", "to_int");
    ZASSERT(!conversion.has_value());
    ZEXPECT(conversion.error().contains("named by the language"));

    auto macro = rename_at("macro", "BOUND");
    ZASSERT(!macro.has_value());
    ZEXPECT(macro.error().contains("macro"));
}

ZEST_CASE(InvalidNewNames) {
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), source);
    merge();

    auto renamed = rename_at("cursor", "target");
    ZASSERT(renamed);
    ZASSERT(renamed->conflicts.size() == 1U);
    ZEXPECT(renamed->conflicts.front().contains("already declared in the same scope"));
}

ZEST_CASE(MacroNameConflict) {
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
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
    ZEXPECT(renamed->unconfirmed[0].reason.contains("preprocessor directive"));
    ZEXPECT(renamed->unconfirmed[0].line == "#define CALL compute(1)");
    ZEXPECT(renamed->unconfirmed[1].reason.contains("written here as `CALL`"));
}

ZEST_CASE(DependentCallHeuristic) {
    add_main(file("a.cpp"), R"(
        int §compute(int x) { return x; }

        template <typename T>
        int apply(T value) { return §compute(value); }
    )");
    merge();

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZASSERT(renamed->edits.size() == 2U);
    ZEXPECT(!renamed->edits[0].heuristic);
    ZEXPECT(renamed->edits[1].heuristic);
}

ZEST_CASE(DependentCallOverloads) {
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
        int compute(int x) { return x; }
        int use() { return compute(1); }
    )");
    merge();
    ZASSERT(!static_cast<bool>(vfs::write(file("a.cpp"), R"(int compute(int x) { return x; }
int later() { return 2; }
)")));

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(renamed->blocked());
    ZASSERT(renamed->stale.size() == 1U);
    ZEXPECT(renamed->stale.front().ends_with("a.cpp"));
}

ZEST_CASE(EditedFileMovedOn) {
    // Non-ASCII text is kept with the rows, so no disk read vouches for it.
    add_main(file("a.cpp"), R"(
        // café
        int compute(int x) { return x; }
    )");
    merge();
    ZASSERT(!static_cast<bool>(vfs::write(file("a.cpp"), R"(// café
int renamed(int x) { return x; }
)")));

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(renamed->blocked());
    ZASSERT(renamed->stale.size() == 1U);
    ZEXPECT(renamed->stale.front().ends_with("a.cpp"));
}

ZEST_CASE(NewNameInChangedFile) {
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
    ZASSERT(!static_cast<bool>(vfs::write(file("b.cpp"), R"(int other() { return 0; }
int evaluate;
)")));

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
    add_main(file("a.cpp"), R"(
        #include "config"
        int use() { return §compute(1); }
    )");
    merge();
    std::erase(scope, file("config"));

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(clean(*renamed));
}

ZEST_CASE(FilesOutsideTheIndex) {
    add_main(file("a.cpp"), R"(
        int §compute(int x) { return x; }
    )");
    merge();
    write("notes.cpp", "int other() { return compute(3); }\n");

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(!renamed->blocked());
    ZASSERT(renamed->unconfirmed.size() == 1U);
    ZEXPECT(renamed->unconfirmed.front().reason.contains("holds no rows"));

    // A unit of the build the index has yet to reach may include it.
    units_pending = true;
    renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZEXPECT(renamed->blocked());
    ZASSERT(renamed->stale.size() == 1U);
    ZEXPECT(renamed->stale.front().ends_with("notes.cpp"));
}

ZEST_CASE(LocalsStayHome) {
    add_file(file("b.h"), R"(
        #define ROWS rows
        #if 0
        int stale = rows;
        #endif
    )");
    add_main(file("a.cpp"), R"(
        #include "b.h"
        int count(int §(cursor)§rows) { return §rows; }
    )");
    merge();
    write("notes.cpp", "int rows = 3;\n");
    units_pending = true;

    // Another file reaches a parameter only through a macro it defines.
    auto renamed = rename_at("cursor", "entries");
    ZASSERT(renamed);
    ZEXPECT(edits(*renamed) == marks());
    ZEXPECT(!renamed->blocked());
    ZASSERT(renamed->unconfirmed.size() == 1U);
    ZEXPECT(renamed->unconfirmed.front().site.path.ends_with("b.h"));
    ZEXPECT(renamed->unconfirmed.front().reason.contains("preprocessor directive"));

    // One changed since it was indexed may define the new name by now.
    ZASSERT(!static_cast<bool>(vfs::write(file("b.h"), "#define entries 1\n#define ROWS rows\n")));
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
    add_main(file("a.cpp"), R"(
        #include "vendor/lib.h"
        int use() { return compute(1); }
    )");
    merge();
    std::erase(scope, file("vendor/lib.h"));
    foreign.insert(file("vendor/lib.h"));

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZASSERT(renamed->conflicts.size() == 1U);
    ZEXPECT(renamed->conflicts.front().contains("not a workspace source"));
}

ZEST_CASE(ApplyRewritesTokens) {
    add_main(file("a.cpp"), R"(
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
    ZEXPECT(!index::apply_renames("int x;", renamed, main_id).has_value());
    auto longer = sources.all_files[file("a.cpp")].content;
    longer.insert(renamed.edits.front().edit.site.range.end, "d");
    ZEXPECT(!index::apply_renames(longer, renamed, main_id).has_value());
}

ZEST_CASE(DollarNames) {
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
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
    add_main(file("a.cpp"), R"(
        int compute(int x) { return x; }
        #define NOTE "é" compute
    )");
    merge();

    auto renamed = rename("compute", "evaluate");
    ZASSERT(renamed);
    ZASSERT(renamed->unconfirmed.size() == 1U);
    auto& begin = renamed->unconfirmed.front().site.begin;
    ZEXPECT(begin.utf16_column + 1 == begin.column);
}

ZEST_CASE(CursorOffTheName) {
    add_main(file("a.cpp"), R"(
        struct Widget { Widget(int); };
        int compute(int x) { return x; }
        #define CALL compute(1)
        int use() { return §(macro)CALL; }
        Widget made§(paren)(1);
    )");
    merge();

    auto macro = rename_at("macro", "evaluate");
    ZASSERT(!macro.has_value());
    ZEXPECT(macro.error().contains("macro"));

    auto paren = rename_at("paren", "Gadget");
    ZASSERT(!paren.has_value());
    ZEXPECT(paren.error().contains("no name of `Widget`"));
}

};  // ZEST_SUITE(Rename)

}  // namespace
}  // namespace clice::testing
