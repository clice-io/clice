module clice;

import :compile.compilation;
import :compile.directive;
import :syntax.header_context;
import :syntax.preamble;
import :unittests.utils;
import :unittests.utils.temp_dir;
import :unittests.utils.tester;
import :vfs.file_system;
import :vfs.path;

namespace clice::testing {

namespace {

/// What the context tests cut from host.cpp, which includes body.h between
/// the open and the close of a class: its text before the class, the
/// class's opening, its include of body.h, and the class's last member
/// and closing.
constexpr llvm::StringRef host_prefix = "#include \"dep.h\"\n";
constexpr llvm::StringRef host_open = "struct S {\n";
constexpr llvm::StringRef host_include = "#include \"body.h\"\n";
constexpr llvm::StringRef host_suffix = "int later;\n};\n";

/// The #line marker a context puts ahead of the text it copies.
constexpr llvm::StringRef marker = "#line 1 \"host.cpp\"\n";

/// What a unit's derivation reads: it counts its builds, and reads the AST
/// when it is released.
struct Reader {
    explicit Reader(CompilationUnit& unit) : first(unit.top_level_decls().front()) {
        built += 1;
    }

    ~Reader() {
        released = llvm::cast<clang::NamedDecl>(first)->getName().str();
    }

    const clang::Decl* first;

    inline static int built = 0;

    inline static std::string released;
};

ZEST_SUITE(CompilationUnit, Tester) {

/// The id of the file `name` of the test.
clang::FileID file(llvm::StringRef name) {
    return unit->file_id(TestVFS::path(name));
}

/// The names of `decls`, template arguments included.
std::vector<std::string> names(llvm::ArrayRef<const clang::Decl*> decls) {
    std::vector<std::string> result;
    auto& policy = unit->context().getPrintingPolicy();
    for(const auto* decl: decls) {
        llvm::raw_string_ostream out(result.emplace_back());
        llvm::cast<clang::NamedDecl>(decl)->getNameForDiagnostic(out, policy, false);
    }
    return result;
}

/// Compile `text` as body.h in host.cpp's context, the way a header context
/// serves it: the prefix the command -includes, behind a marker; the
/// opening of the class, entered next; the suffix after the text.
bool compile_in_context(llvm::StringRef text) {
    add_file("dep.h", "int dep;\n");
    add_focus("body.h", text);
    prepare();
    params.input.add_focus(TestVFS::path("body.h"), text);
    auto host = TestVFS::path("host.cpp");
    auto prefix = TestVFS::path(".clice-prefix.h");
    auto run = [](std::size_t offset, std::size_t source_offset, std::size_t length) {
        return SourceRun{.offset = static_cast<std::uint32_t>(offset),
                         .source_offset = static_cast<std::uint32_t>(source_offset),
                         .length = static_cast<std::uint32_t>(length)};
    };
    params.input.add_synthesized({
        {.path = prefix,
         .content = std::format("{}{}", marker, host_prefix),
         .origin = {.source = host, .runs = {run(marker.size(), 0, host_prefix.size())}},
         .entry = SynthesizedFile::Entry::Prefix},
        {.path = TestVFS::path(".clice-open.h"),
         .content = host_open.str(),
         .origin = {.source = host, .runs = {run(0, host_prefix.size(), host_open.size())}},
         .entry = SynthesizedFile::Entry::Forced},
        {.path = TestVFS::path(".clice-suffix.h"),
         .content = host_suffix.str(),
         .origin = {.source = host,
                    .runs = {run(0,
                                 host_prefix.size() + host_open.size() + host_include.size(),
                                 host_suffix.size())}},
         .entry = SynthesizedFile::Entry::Appended},
    });
    params.input.arguments.insert(params.input.arguments.end() - 1, {"-include", prefix});
    if(!try_compile()) {
        return false;
    }
    ZEXPECT(unit->diagnostics().empty());
    return true;
}

ZEST_CASE(FileIds) {
    add_files("main.cpp", R"cpp(
#[once.h]
#pragma once
int once;
#[twice.h]
extern int twice;
#[unused.h]
#[main.cpp]
#include "once.h"
#include "once.h"
#include "twice.h"
#include "twice.h"
#define ZERO 0
int zero = ZERO;
)cpp");
    ZASSERT(compile());
    auto focus = unit->focus_file().file();
    auto& includes = unit->directives()[focus].includes;
    ZASSERT(includes.size() == 4U);
    auto& SM = unit->context().getSourceManager();

    ZEXPECT(file("once.h") == includes[0].fid);
    ZEXPECT(unit->file_id(declared("once")->getLocation()) == includes[0].fid);

    // A file entered twice has an id each time; the file's is the first.
    ZEXPECT(includes[2].fid != includes[3].fid);
    ZEXPECT(file("twice.h") == includes[2].fid);
    ZEXPECT(unit->file_id(*SM.getFileEntryRefForID(includes[3].fid)) == includes[2].fid);

    ZEXPECT(file("unused.h").isInvalid());
    ZEXPECT(file("missing.h").isInvalid());

    ZEXPECT(unit->file_id(declared("zero")->getLocation()) == focus);
    auto expansion = unit->file_id(declared("zero")->getInit()->getBeginLoc());
    ZEXPECT(expansion != focus);
    ZEXPECT(SM.getSLocEntry(expansion).isExpansion());
}

ZEST_CASE(FilePaths) {
    add_file("proj/inc/deep.h", "int deep;\n");
    add_focus("proj/main.cpp", "#include <deep.h>\n#if __has_include(<deep.h>)\n#endif\n");
    prepare();
    params.input.directory = TestVFS::path("proj");
    params.input.arguments.insert(params.input.arguments.end() - 1, "-Iinc");
    ZASSERT(try_compile());
    auto& directive = unit->directives()[unit->focus_file().file()];
    ZASSERT(directive.includes.size() == 1U);
    ZASSERT(directive.has_includes.size() == 1U);
    ZASSERT(directive.has_includes[0].file);

    // `-Iinc` finds the header relative to the compile's directory.
    auto path = unit->file_path(directive.includes[0].fid);
    ZEXPECT(path == TestVFS::path("proj/inc/deep.h"));
    ZEXPECT(path.data()[path.size()] == '\0');
    ZEXPECT(unit->file_path(*directive.has_includes[0].file) == TestVFS::path("proj/inc/deep.h"));
    ZEXPECT(unit->file_path(unit->focus_file().file()) == TestVFS::path("proj/main.cpp"));
}

ZEST_CASE(FilePathResolvesLinks, skip = Windows) {
    TempDir tmp;
    tmp.touch("real/a.h", "extern int a;\n");
    ZASSERT(!fs::create_symlink(tmp.path("real/a.h"), tmp.path("link.h")));
    tmp.touch("main.cpp", "#include \"link.h\"\n#if __has_include(\"link.h\")\n#endif\n");
    prepare();
    params.input.vfs = llvm::makeIntrusiveRefCnt<vfs::View>();
    params.input.arguments.back() = tmp.path("main.cpp");
    ZASSERT(try_compile());
    auto& directive = unit->directives()[unit->focus_file().file()];
    ZASSERT(directive.includes.size() == 1U);
    ZASSERT(directive.has_includes.size() == 1U);
    ZASSERT(directive.has_includes[0].file);
    ZEXPECT(unit->file_path(directive.includes[0].fid) == tmp.path("real/a.h"));
    ZEXPECT(unit->file_path(*directive.has_includes[0].file) == tmp.path("real/a.h"));
}

ZEST_CASE(FilePathOfHardLinks) {
    TempDir tmp;
    tmp.touch("a.h", "extern int a;\n");
    ZASSERT(!fs::create_hard_link(tmp.path("a.h"), tmp.path("b.h")));
    tmp.touch("main.cpp", "#include \"a.h\"\n#include \"b.h\"\n");
    prepare();
    params.input.vfs = llvm::makeIntrusiveRefCnt<vfs::View>();
    params.input.arguments.back() = tmp.path("main.cpp");
    ZASSERT(try_compile());
    auto& includes = unit->directives()[unit->focus_file().file()].includes;
    ZASSERT(includes.size() == 2U);
    auto first = CanonicalPath(Spelling::absolute(tmp.path("a.h")));
    ZEXPECT(unit->file_path(includes[0].fid) == first.str());
    ZEXPECT(unit->file_path(includes[1].fid) == first.str());
}

ZEST_CASE(ProjectRoot) {
    add_focus("main.cpp", "int x;\n");
    prepare();
    params.input.project_root = TestVFS::path("project");
    ZASSERT(try_compile());
    ZEXPECT(unit->project_root() == TestVFS::path("project"));
}

ZEST_CASE(OtherFileText) {
    add_files("main.cpp", R"cpp(
#[a.h]
§(line)⟦// one⟧
int a; §(block)⟦/* two */⟧
int §(b)b;
#[main.cpp]
#include "a.h"
)cpp");
    ZASSERT(compile());
    auto header = file("a.h");
    ZEXPECT(unit->file_content(header) == sources.all_files["a.h"].content);

    auto position = unit->positions(header).position(point("b", "a.h"), PositionEncoding::UTF8);
    ZASSERT(position);
    ZEXPECT(position->line == 2U);
    ZEXPECT(position->character == 4U);

    auto comments = unit->comments(header);
    ZASSERT(comments.size() == 2U);
    ZEXPECT(comments[0].kind == LexicalInfo::Comment::Kind::Line);
    ZEXPECT(comments[0].range == range("line", "a.h"));
    ZEXPECT(comments[1].kind == LexicalInfo::Comment::Kind::Block);
    ZEXPECT(comments[1].range == range("block", "a.h"));

    auto& lexical = unit->lexical(header);
    ZEXPECT(lexical.comments.size() == 2U);
    auto code = lexical.code_from(0);
    ZASSERT(!code.empty());
    ZEXPECT(code.front().text(unit->file_content(header)) == "int");
}

ZEST_CASE(FocusTextAsEdited) {
    llvm::StringRef text = "// note\nint sum() { return a + later; }\nint a;";
    ZASSERT(compile_in_context(text));
    auto focus = unit->focus_file();
    ZEXPECT(unit->file_content(focus.file()) == text);
    ZEXPECT(focus.content() == text);
    ZEXPECT(unit->loaded_file_content(focus.file())->size() > text.size());
    ZEXPECT(unit->positions(focus.file()).size() == text.size());
    ZEXPECT(focus.positions().size() == text.size());

    // Clang reads the include of the suffix past the text, which is no line
    // of it.
    ZEXPECT(focus.lexical().directives.empty());
    ZEXPECT(&unit->lexical(focus.file()) == &focus.lexical());
    auto comments = unit->comments(focus.file());
    ZASSERT(comments.size() == 1U);
    ZEXPECT(comments[0].range == LocalSourceRange{0, 7});
}

ZEST_CASE(FromContext) {
    ZASSERT(compile_in_context("int a;\n"));
    ZEXPECT(unit->from_context(file(".clice-prefix.h")));
    ZEXPECT(unit->from_context(file(".clice-open.h")));
    ZEXPECT(unit->from_context(file(".clice-suffix.h")));
    ZEXPECT(unit->from_context(file("dep.h")));
    ZEXPECT(!unit->from_context(unit->focus_file().file()));

    clear();
    add_file("a.h", "int a;\n");
    add_focus("main.cpp", "#include \"a.h\"\n");
    ZASSERT(compile());
    ZEXPECT(!unit->from_context(file("a.h")));
}

ZEST_CASE(FragmentPath) {
    ZASSERT(compile_in_context("int a;\n"));
    auto open = file(".clice-open.h");
    auto& SM = unit->context().getSourceManager();
    ZEXPECT(unit->file_path(open) == TestVFS::path("host.cpp"));
    ZEXPECT(unit->file_path(*SM.getFileEntryRefForID(open)) == TestVFS::path("host.cpp"));
    ZEXPECT(unit->file_path(file("dep.h")) == TestVFS::path("dep.h"));
    ZEXPECT(unit->file_path(unit->focus_file().file()) == TestVFS::path("body.h"));
}

ZEST_CASE(SourceOf) {
    ZASSERT(compile_in_context("int a;\n"));
    auto prefix = file(".clice-prefix.h");
    auto host = TestVFS::path("host.cpp");

    ZEXPECT(!unit->source_of({prefix, 2}));
    auto copied = unit->source_of({prefix, static_cast<std::uint32_t>(marker.size())});
    ZASSERT(copied);
    ZEXPECT(copied->path == host);
    ZEXPECT(copied->offset == 0U);
    auto end =
        unit->source_of({prefix, static_cast<std::uint32_t>(marker.size() + host_prefix.size())});
    ZASSERT(end);
    ZEXPECT(end->offset == host_prefix.size());

    auto name = unit->source_of(unit->seen_at(declared<clang::CXXRecordDecl>("S")->getLocation()));
    ZASSERT(name);
    ZEXPECT(name->path == host);
    ZEXPECT(name->offset == host_prefix.size() + host_open.find('S'));

    auto own = unit->source_of({unit->focus_file().file(), 4});
    ZASSERT(own);
    ZEXPECT(own->path == TestVFS::path("body.h"));
    ZEXPECT(own->offset == 4U);
}

ZEST_CASE(EnclosesFocusFile) {
    ZASSERT(compile_in_context("int a;\n"));
    const auto* record = declared<clang::CXXRecordDecl>("S");
    ZEXPECT(unit->encloses_focus_file(record));
    ZEXPECT(!unit->encloses_focus_file(declared("dep")));
    for(const auto* field: record->fields()) {
        ZEXPECT(!unit->encloses_focus_file(field));
    }
}

ZEST_CASE(PreambleFiles) {
    add_files("main.cpp", R"cpp(
#[a.h]
#include "c.h"
#[c.h]
int c;
#[b.h]
int b;
#[forced.h]
int forced;
#[main.cpp]
#include "a.h"
int x;
#include "b.h"
)cpp");
    prepare();
    params.input.arguments.insert(params.input.arguments.end() - 1,
                                  {"-include", TestVFS::path("forced.h")});
    ZASSERT(try_compile());
    ZEXPECT(unit->in_preamble(file("forced.h")));
    ZEXPECT(unit->in_preamble(file("a.h")));
    ZEXPECT(unit->in_preamble(file("c.h")));
    ZEXPECT(!unit->in_preamble(file("b.h")));
    ZEXPECT(!unit->in_preamble(unit->focus_file().file()));
}

ZEST_CASE(LoadedFileContent) {
    add_files("main.cpp", R"cpp(
#[a.h]
int a;
#[b.h]
int b;
#[main.cpp]
#include "a.h"
int x;
#include "b.h"
)cpp");
    ZASSERT(compile_with_pch());
    // The PCH served a.h: this compile never read it.
    ZEXPECT(!unit->loaded_file_content(file("a.h")));
    ZEXPECT(unit->loaded_file_content(file("b.h")) == "int b;\n");
}

ZEST_CASE(PrecompiledHeaderWhole) {
    add_files("whole.h", R"cpp(
#[early.h]
int early;
#[late.h]
int late;
#[whole.h]
#include "early.h"
int between;
#include "late.h"
)cpp");
    prepare();
    params.kind = CompilationKind::PrecompiledHeader;
    params.output_file = precompiled_path("pch");
    *(params.input.arguments.end() - 2) = "c++-header";
    ZASSERT(try_compile());
    ZEXPECT(unit->in_preamble(file("late.h")));
}

ZEST_CASE(ContextPreamble) {
    // The prefix is the context's part ahead of the braces the header sits
    // in, the open fragment and the suffix the part inside.
    ZASSERT(compile_in_context("int a;\n"));
    ZEXPECT(unit->in_preamble(file(".clice-prefix.h")));
    ZEXPECT(unit->in_preamble(file("dep.h")));
    ZEXPECT(!unit->in_preamble(file(".clice-open.h")));
    ZEXPECT(!unit->in_preamble(file(".clice-suffix.h")));
}

ZEST_CASE(SystemHeaders) {
    add_file("sys/lib.h", "int library;\n");
    add_focus("main.cpp", "#include <lib.h>\nint own;\n");
    prepare();
    params.input.arguments.insert(params.input.arguments.end() - 1,
                                  {"-isystem", TestVFS::path("sys")});
    ZASSERT(try_compile());
    auto library = declared("library")->getLocation();
    ZEXPECT(unit->in_system_header(library));
    ZEXPECT(unit->in_library(library));
    auto own = declared("own")->getLocation();
    ZEXPECT(!unit->in_system_header(own));
    ZEXPECT(!unit->in_library(own));
}

ZEST_CASE(SystemHeaderAsFocus) {
    // The nearest search directory above a header compiled as the focus file
    // tells whether the files including it see a system header.
    auto compile_header = [&](llvm::StringRef focus, llvm::StringRef other) {
        clear();
        add_file(other, "int other;\n");
        add_focus(focus, "int own;\n");
        prepare();
        auto& arguments = params.input.arguments;
        *(arguments.end() - 2) = "c++-header";
        arguments.insert(arguments.end() - 1,
                         {"-isystem", TestVFS::path("sys"), "-I", TestVFS::path("sys/app")});
        return try_compile();
    };

    ZASSERT(compile_header("sys/lib.h", "sys/app/app.h"));
    auto own = declared("own")->getLocation();
    ZEXPECT(unit->in_system_header(own));
    ZEXPECT(!unit->in_library(own));

    ZASSERT(compile_header("sys/app/app.h", "sys/lib.h"));
    ZEXPECT(!unit->in_system_header(declared("own")->getLocation()));
}

ZEST_CASE(LibraryPastLineMarker) {
    add_focus("main.ii", R"cpp(int own;
# 1 "/usr/include/lib.h" 1 3
int library;
)cpp");
    ZASSERT(compile());
    ZEXPECT(!unit->in_library(declared("own")->getLocation()));
    ZEXPECT(unit->in_library(declared("library")->getLocation()));
}

ZEST_CASE(BuiltinFiles) {
    add_files("main.cpp", R"cpp(
#[a.h]
int header;
#[main.cpp]
#include "a.h"
#define CAT(a, b) a##b
int CAT(pas, ted) = FROM_COMMAND;
)cpp");
    prepare();
    params.input.arguments.insert(params.input.arguments.end() - 1, "-DFROM_COMMAND=1");
    ZASSERT(try_compile());
    auto definitions = unit->macro_definitions("FROM_COMMAND");
    ZASSERT(definitions.size() == 1U);
    ZEXPECT(unit->is_builtin_file(unit->file_id(definitions[0]->getDefinitionLoc())));
    ZEXPECT(unit->is_builtin_file(unit->written_at(declared("pasted")->getLocation()).file));
    ZEXPECT(!unit->is_builtin_file(unit->focus_file().file()));
    ZEXPECT(!unit->is_builtin_file(file("a.h")));
}

ZEST_CASE(IncludeLocation) {
    add_files("main.cpp", R"cpp(
#[a.h]
int header;
#[main.cpp]
§(include)⟦#include "a.h"⟧
int own = FROM_COMMAND;
)cpp");
    prepare();
    params.input.arguments.insert(params.input.arguments.end() - 1, "-DFROM_COMMAND=1");
    ZASSERT(try_compile());
    auto focus = unit->focus_file().file();
    auto included = unit->seen_at(unit->include_location(file("a.h")));
    ZEXPECT(included.file == focus);
    ZEXPECT(range("include").begin <= included.offset);
    ZEXPECT(included.offset < range("include").end);
    ZEXPECT(!unit->imported(file("a.h")));

    ZEXPECT(unit->include_location(focus).isInvalid());
    ZEXPECT(!unit->imported(focus));
    auto definitions = unit->macro_definitions("FROM_COMMAND");
    ZASSERT(definitions.size() == 1U);
    ZEXPECT(unit->include_location(unit->file_id(definitions[0]->getDefinitionLoc())).isInvalid());
}

ZEST_CASE(Imported) {
    add_files("main.cpp", R"cpp(
#[mod.cppm]
export module mod;
export int exported();
#[main.cpp]
§(import)⟦import mod;⟧
int use = exported();
)cpp");
    ZASSERT(compile_with_modules());
    auto module = unit->file_id(declared<clang::FunctionDecl>("exported")->getLocation());
    ZEXPECT(unit->imported(module));
    auto loaded = unit->seen_at(unit->include_location(module));
    ZEXPECT(loaded.file == unit->focus_file().file());
    ZEXPECT(range("import").begin <= loaded.offset);
    ZEXPECT(loaded.offset < range("import").end);

    // A header a PCH brings in came with no import.
    clear();
    add_files("main.cpp", R"cpp(
#[a.h]
int header;
#[main.cpp]
#include "a.h"
int use = header;
)cpp");
    ZASSERT(compile_with_pch());
    ZEXPECT(!unit->imported(unit->file_id(declared("header")->getLocation())));
}

ZEST_CASE(ModuleNames) {
    add_focus("main.cpp", "int x;\n");
    ZASSERT(compile());
    ZEXPECT(!unit->is_named_module());
    ZEXPECT(!unit->defines_module());

    clear();
    add_focus("part.cppm", "export module app.core:part;\n");
    ZASSERT(compile());
    ZEXPECT(unit->is_named_module());
    ZEXPECT(unit->module_name() == "app.core:part");
    ZEXPECT(unit->defines_module());

    clear();
    add_focus("detail.cpp", "module app.core:detail;\n");
    ZASSERT(compile());
    ZEXPECT(unit->is_named_module());
    ZEXPECT(unit->module_name() == "app.core:detail");
    ZEXPECT(unit->defines_module());

    clear();
    add_file("app.cppm", "export module app;\n");
    add_focus("impl.cpp", "module app;\n");
    ZASSERT(compile_with_modules());
    ZEXPECT(unit->is_named_module());
    ZEXPECT(unit->module_name() == "app");
    ZEXPECT(!unit->defines_module());
}

ZEST_CASE(MacroDefinitions) {
    add_files("main.cpp", R"cpp(
#[a.h]
#define LEVEL 1
#[main.cpp]
#include "a.h"
#undef LEVEL
#define LEVEL 2
int x = LEVEL;
#undef LEVEL
#define LEVEL 3
)cpp");
    ZASSERT(compile_with_pch());
    std::vector<std::string> values;
    for(const auto* definition: unit->macro_definitions("LEVEL")) {
        values.push_back(unit->token_spelling(definition->getReplacementToken(0).getLocation()));
    }
    ZEXPECT(values == std::vector<std::string>{"1", "2", "3"});
    ZEXPECT(unit->macro_definitions("ABSENT").empty());
}

ZEST_CASE(MacroInvocations) {
    add_files("main.cpp", R"cpp(
#[a.h]
#define HEADER 1
#[main.cpp]
#include "a.h"
#define ONE 1
#define ID(x) x
#define TWO (ONE + ONE)
#define EMPTY
EMPTY int a = §(a)ONE;
int b = §(b)ID(§(inner)ONE);
int c = §(c)TWO;
int d = HEADER;
int e = §(e)ID(HEADER);
)cpp");
    ZASSERT(compile());
    std::vector<std::uint32_t> offsets;
    for(auto location: unit->focus_file().macro_invocations()) {
        offsets.push_back(unit->seen_at(location).offset);
    }
    ZEXPECT(offsets == std::vector{point("a"), point("b"), point("inner"), point("c"), point("e")});
}

ZEST_CASE(TopLevelDecls) {
    add_files("main.cpp", R"cpp(
#[a.h]
int header;
#[main.cpp]
int a;
#include "a.h"
void f();
struct B {};
)cpp");
    ZASSERT(compile());
    ZEXPECT(names(unit->top_level_decls()) == std::vector<std::string>{"a", "f", "B"});

    // The class a header context opens around the text encloses it.
    clear();
    ZASSERT(compile_in_context("int a;\n"));
    ZEXPECT(names(unit->top_level_decls()) == std::vector<std::string>{"S"});
}

ZEST_CASE(GlobalModuleFragmentFirst) {
    add_files("main.cpp", R"cpp(
#[a.h]
int header;
#[main.cpp]
module;
#include "a.h"
int own;
export module m;
int body;
)cpp");
    ZASSERT(compile_with_pch());
    ZEXPECT(names(unit->top_level_decls()) == std::vector<std::string>{"own", "body"});
}

ZEST_CASE(Instantiations) {
    // A class is instantiated where it is used, a function's or a variable's
    // definition at the end of the unit.
    add_focus("main.cpp", R"cpp(
template <typename T>
T twice(T value) {
    return value + value;
}

template <typename T>
struct Box {
    T value;
};

template <typename T>
T zero = T();

int four = twice(2);
Box<int> box;
long none = zero<long>;
template struct Box<char>;
)cpp");
    ZASSERT(compile());
    ZEXPECT(names(unit->instantiations()) ==
            std::vector<std::string>{"Box<int>", "Box<char>", "twice<int>", "zero<long>"});
}

ZEST_CASE(InstantiationsBehindPreamble) {
    add_file("show.h", R"cpp(
#pragma once
template <typename T>
struct Box {
    T value;
};

inline Box<int> boxed;

template <typename T>
void show(T value) {
    describe(value);
}

namespace app {
struct Point {};
}  // namespace app

inline void demo() {
    show(app::Point{});
}
)cpp");
    // Where the preamble ends show<app::Point> finds no describe: the PCH
    // leaves its body pending.
    add_focus("main.cpp", R"cpp(
#include "show.h"

namespace app {
void describe(Point) {}
}  // namespace app
)cpp");
    ZASSERT(compile_with_pch());
    ZEXPECT(collect_errors(*unit).empty());
    ZEXPECT(names(unit->instantiations()) == std::vector<std::string>{"show<app::Point>"});
}

ZEST_CASE(UnindexedInstantiateNothing) {
    llvm::StringRef text = R"cpp(#include "box.h"
Box<int> box;
int x = bo;
)cpp";
    add_file("box.h", R"cpp(
template <typename T>
struct Box {
    T value;
};

inline Box<long> boxed;
)cpp");
    add_focus("main.cpp", text);
    ZASSERT(compile());
    ZEXPECT(!unit->instantiations().empty());

    prepare();
    params.input.add_focus(TestVFS::path("main.cpp"), text);
    params.kind = CompilationKind::Completion;
    params.completion = {
        .offset = static_cast<std::uint32_t>(text.find("bo;") + 2),
        .consumer =
            [](CompilationUnit&) {
                return std::make_unique<clang::PrintingCodeCompleteConsumer>(
                    clang::CodeCompleteOptions(),
                    llvm::nulls());
            },
    };
    ZASSERT(try_compile());
    ZEXPECT(unit->instantiations().empty());

    prepare();
    params.kind = CompilationKind::Preamble;
    params.output_file = precompiled_path("pch");
    params.input.add_remapped_file(TestVFS::path("main.cpp"), text, preamble_bound(text));
    auto preamble = clice::compile(std::move(params));
    ZASSERT(preamble->has_output());
    ZEXPECT(preamble->instantiations().empty());
}

ZEST_CASE(Keywords) {
    auto keyword = [&](llvm::StringRef word, clang::tok::TokenKind kind) {
        auto& table = unit->keywords();
        auto found = table.find(word);
        return found != table.end() && found->getValue()->getTokenID() == kind;
    };

    // The suite's commands carry -fms-extensions.
    add_focus("main.cpp", "class Widget {};\n");
    ZASSERT(compile());
    ZEXPECT(keyword("class", clang::tok::kw_class));
    ZEXPECT(keyword("__declspec", clang::tok::kw___declspec));
    auto focus = unit->focus_file();
    ZEXPECT(focus.kind(focus.lexical().tokens[0]) == clang::tok::kw_class);
    ZEXPECT(focus.kind(focus.lexical().tokens[1]) == clang::tok::identifier);

    clear();
    add_focus("main.c", "int class;\n");
    prepare("-std=c17");
    std::erase(params.input.arguments, "-fms-extensions");
    ZASSERT(try_compile());
    ZEXPECT(!keyword("class", clang::tok::kw_class));
    ZEXPECT(!keyword("__declspec", clang::tok::kw___declspec));
    auto c = unit->focus_file();
    ZEXPECT(c.kind(c.lexical().tokens[1]) == clang::tok::identifier);
}

ZEST_CASE(DerivedOnce) {
    add_focus("main.cpp", "int first;\n");
    ZASSERT(compile());
    Reader::built = 0;
    auto& reader = unit->get<Reader>();
    ZEXPECT(&unit->get<Reader>() == &reader);
    ZEXPECT(Reader::built == 1);

    // Released with the unit, while the AST it reads is still there.
    unit.reset();
    ZEXPECT(Reader::released == "first");
}

ZEST_CASE(ParsedByPCH) {
    llvm::StringRef files = R"cpp(
#[a.h]
int header;
#[main.cpp]
#include "a.h"
#define VALUE 1
int value = VALUE;
)cpp";
    add_files("main.cpp", files);
    ZASSERT(compile_with_pch());
    auto focus = unit->focus_file();
    auto definitions = unit->macro_definitions("VALUE");
    ZASSERT(definitions.size() == 1U);
    ZEXPECT(focus.parsed_by_pch(definitions[0]->getDefinitionLoc()));
    // `1` is written in the preamble and seen past it, where VALUE is.
    ZEXPECT(!focus.parsed_by_pch(declared("value")->getInit()->getBeginLoc()));
    ZEXPECT(!focus.parsed_by_pch(declared("value")->getLocation()));
    auto header = declared("header")->getLocation();
    ZEXPECT(!focus.parsed_by_pch(header));
    ZEXPECT(!focus.contains(unit->file_id(header)));

    clear();
    add_files("main.cpp", files);
    ZASSERT(compile());
    definitions = unit->macro_definitions("VALUE");
    ZASSERT(definitions.size() == 1U);
    ZEXPECT(!unit->focus_file().parsed_by_pch(definitions[0]->getDefinitionLoc()));
}

};  // ZEST_SUITE(CompilationUnit)

}  // namespace

}  // namespace clice::testing
