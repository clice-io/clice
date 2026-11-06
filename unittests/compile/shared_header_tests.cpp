module clice;

import :compile.compilation;
import :compile.shared_header;
import :unittests.utils.tester;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {

namespace {

ZEST_SUITE(SharedHeader, Tester) {

struct Compiled {
    std::shared_ptr<const SharedHeaderRecord> record;

    std::unique_ptr<CompilationUnit> unit;
};

/// Build `shared.hxx` the way a shared header is built, under the focus
/// file's command with `extra` too, and compile the focus file loading its
/// PCH, `extra` and `own` added to its command.
Compiled compile_over(Tester& tester,
                      llvm::ArrayRef<std::string> extra = {},
                      llvm::ArrayRef<std::string> own = {}) {
    tester.kind = CompilationKind::Whole;
    tester.prepare();
    auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
        llvm::makeIntrusiveRefCnt<vfs::View>());
    overlay->pushOverlay(tester.vfs);
    auto& input = tester.params.input;
    input.vfs = overlay;
    input.arguments.insert(input.arguments.end() - 1, extra.begin(), extra.end());

    auto pch = tester.precompiled_path("pch");
    CompilationParams header{
        .kind = CompilationKind::PrecompiledHeader,
        .input = input,
        .output_file = pch,
        .record_shared = true,
    };
    auto& arguments = header.input.arguments;
    arguments.back() = TestVFS::path("shared.hxx");
    *ranges::find(arguments, "c++") = "c++-header";
    Compiled compiled;
    {
        auto built = clice::compile(std::move(header));
        ZEXPECT(built->has_output());
        ZEXPECT(collect_errors(*built).empty());
        compiled.record = std::make_shared<SharedHeaderRecord>(built->shared_record());
    }

    CompilationParams unit{.kind = CompilationKind::Whole,
                           .input = input,
                           .shared = compiled.record};
    unit.input.pch = {pch, 0};
    unit.input.arguments.insert(unit.input.arguments.end() - 1, own.begin(), own.end());
    compiled.unit = clice::compile(std::move(unit));
    ZEXPECT(compiled.unit->completed());
    ZEXPECT(collect_errors(*compiled.unit).empty());
    return compiled;
}

/// Whether the unit's `name` was read from a PCH.
bool from_pch(CompilationUnit& unit, llvm::StringRef name) {
    auto found = unit.tu()->lookup(&unit.context().Idents.get(name));
    return !found.empty() && found.front()->isFromASTFile();
}

/// The grafted includes as `file:line`, a skipped one marked, nested ones
/// indented under theirs.
std::vector<std::string> grafted(const Compiled& compiled) {
    std::vector<std::string> lines;
    auto includes = compiled.unit->grafted_includes();
    for(auto& include: includes) {
        std::size_t depth = 0;
        for(auto parent = include.parent; parent != SharedHeaderRecord::none;
            parent = includes[parent].parent) {
            depth += 1;
        }
        lines.push_back(std::format("{}{}:{}{}",
                                    std::string(depth * 2, ' '),
                                    path::filename(compiled.record->files[include.file].path),
                                    include.line,
                                    include.skipped ? " skipped" : ""));
    }
    return lines;
}

ZEST_CASE(IncludedAsBuilt) {
    add_file("shared.hxx", "#pragma once\n#include \"outer.h\"\n");
    add_file("inner.h", "#pragma once\n#define INNER 1\nint inner();\n");
    add_file("outer.h", "#pragma once\n#include \"inner.h\"\nint outer() { return INNER; }\n");
    add_focus("main.cpp", R"cpp(#include "outer.h"
#include "inner.h"
int use() { return outer() + inner() + INNER; }
)cpp");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().empty());
    ZEXPECT(from_pch(*compiled.unit, "outer"));
    ZEXPECT(grafted(compiled) ==
            std::vector<std::string>{"outer.h:1", "  inner.h:2", "inner.h:2 skipped"});
};

ZEST_CASE(OrderOfTheUnit) {
    // The PCH enters first.h before second.h, the unit the other way
    // round: what each includes is grafted where the unit enters it.
    add_file("shared.hxx", "#pragma once\n#include \"first.h\"\n#include \"second.h\"\n");
    add_file("common.h", "#pragma once\nint common();\n");
    add_file("first.h", "#pragma once\n#include \"common.h\"\nint first();\n");
    add_file("second.h", "#pragma once\n#include \"common.h\"\nint second();\n");
    add_focus("main.cpp", "#include \"second.h\"\n#include \"first.h\"\n");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().empty());
    ZEXPECT(grafted(compiled) == std::vector<std::string>{"second.h:1",
                                                          "  common.h:2",
                                                          "first.h:2",
                                                          "  common.h:2 skipped"});
};

ZEST_CASE(OwnMacroHeaderReads) {
    add_file("shared.hxx", "#pragma once\n#include \"config.h\"\n");
    add_file("config.h", R"cpp(#pragma once
#ifdef WIDE
using size = long;
#else
using size = int;
#endif
)cpp");
    add_focus("main.cpp", "#define WIDE\n#include \"config.h\"\nsize value = 0;\n");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("WIDE"));
    // Parsed again without the PCH.
    ZEXPECT(!from_pch(*compiled.unit, "size"));
};

ZEST_CASE(OwnMacroReplacesIdentifier) {
    // An identifier is a read too: the unit's macro would have replaced it.
    add_file("shared.hxx", "#pragma once\n#include \"names.h\"\n");
    add_file("names.h", "#pragma once\nstruct widget { int field; };\n");
    add_focus("main.cpp", "#define field renamed\n#include \"names.h\"\n");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("field"));
};

ZEST_CASE(ReadBeforeIncluding) {
    add_file("shared.hxx", "#pragma once\n#include \"limits.h\"\n");
    add_file("limits.h", "#pragma once\n#define LIMIT 8\n");
    add_focus("main.cpp", R"cpp(#ifdef LIMIT
int early;
#endif
#include "limits.h"
)cpp");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("LIMIT"));
    ZEXPECT(!from_pch(*compiled.unit, "early"));
};

ZEST_CASE(OwnHeldMacroDefault) {
    // With the PCH the unit's own definition would be an inactive branch.
    add_file("shared.hxx", "#pragma once\n#include \"limits.h\"\n");
    add_file("limits.h", "#pragma once\n#define LIMIT 8\nint limit();\n");
    add_focus("main.cpp", R"cpp(#ifndef LIMIT
#define LIMIT 8
#endif
#include "limits.h"
)cpp");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("LIMIT"));
};

ZEST_CASE(NeverIncluded) {
    add_file("shared.hxx", "#pragma once\n#include \"used.h\"\n#include \"unused.h\"\n");
    add_file("used.h", "#pragma once\nint used();\n");
    add_file("unused.h", "#pragma once\nint unused();\n");
    add_focus("main.cpp", "#include \"used.h\"\n");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("unused.h"));
};

ZEST_CASE(SystemOrder) {
    // Two headers of the standard library read and write a macro only
    // they touch, the unit including them the other way round: accepted.
    add_file("shared.hxx", "#pragma once\n#include <reader.h>\n#include <writer.h>\n");
    add_file("sys/reader.h", R"cpp(#pragma once
#ifndef __size_defined
#define __size_defined
typedef unsigned long size_type;
#endif
)cpp");
    add_file("sys/writer.h", R"cpp(#pragma once
#ifndef __size_defined
#define __size_defined
typedef unsigned long size_type;
#endif
)cpp");
    add_focus("main.cpp", "#include <writer.h>\n#include <reader.h>\n");
    auto compiled = compile_over(*this, {"-internal-isystem", TestVFS::path("sys")});
    ZEXPECT(compiled.unit->shared_rejection().empty());
};

ZEST_CASE(ReadAfterAnInclude) {
    // The PCH enters writer.h first, so reader.h's own include of it is
    // skipped there; the unit's text includes reader.h alone, which enters
    // writer.h before reading what it defines.
    add_file("shared.hxx", "#pragma once\n#include \"writer.h\"\n#include \"reader.h\"\n");
    add_file("writer.h", "#pragma once\n#define FAST\n");
    add_file("reader.h", R"cpp(#pragma once
#include "writer.h"
#ifdef FAST
int fast();
#endif
)cpp");
    add_focus("main.cpp", "#include \"reader.h\"\nint use() { return fast(); }\n");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().empty());
    ZEXPECT(grafted(compiled) == std::vector<std::string>{"reader.h:1", "  writer.h:2"});
};

ZEST_CASE(ProjectHeaderSystemRead) {
    // config.h enters the unguarded system header mark.h, which reads the
    // macro its first entry, through base.h, wrote: the order of the system
    // library's own entries.
    add_file("shared.hxx", "#pragma once\n#include <base.h>\n#include \"config.h\"\n");
    add_file("sys/mark.h", R"cpp(#ifdef _MARK_H
#undef _MARK_H
#endif
#define _MARK_H 1
)cpp");
    add_file("sys/base.h", "#pragma once\n#include <mark.h>\n");
    add_file("config.h", "#pragma once\n#include <mark.h>\nint config();\n");
    add_focus("main.cpp", "#include \"config.h\"\n#include <base.h>\n");
    auto compiled = compile_over(*this, {"-internal-isystem", TestVFS::path("sys")});
    ZEXPECT(compiled.unit->shared_rejection().empty());
};

ZEST_CASE(ProjectOrder) {
    // The same between project headers is no system library's business.
    add_file("shared.hxx", "#pragma once\n#include \"reader.h\"\n#include \"writer.h\"\n");
    add_file("reader.h", R"cpp(#pragma once
#ifdef FAST
int fast();
#endif
)cpp");
    add_file("writer.h", "#pragma once\n#define FAST\n");
    add_focus("main.cpp", "#include \"writer.h\"\n#include \"reader.h\"\n");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("FAST"));
};

ZEST_CASE(ForcedIncludes) {
    add_file("shared.hxx", "#pragma once\n#include \"user.h\"\n");
    add_file("forced.h", "#pragma once\n#define FORCED 1\n");
    add_file("user.h", "#pragma once\nint user() { return FORCED; }\n");
    add_focus("main.cpp", "#include \"user.h\"\nint value = FORCED;\n");
    auto compiled = compile_over(*this, {"-include", TestVFS::path("forced.h")});
    ZEXPECT(compiled.unit->shared_rejection().empty());
    ZEXPECT(from_pch(*compiled.unit, "user"));
};

ZEST_CASE(OwnCommandMacro) {
    // A macro the unit's command alone defines, which a held header reads.
    add_file("shared.hxx", "#pragma once\n#include \"config.h\"\n");
    add_file("config.h", R"cpp(#pragma once
#ifdef MODE
int moded();
#endif
)cpp");
    add_focus("main.cpp", "#include \"config.h\"\n");
    auto compiled = compile_over(*this, {}, {"-DMODE"});
    ZEXPECT(compiled.unit->shared_rejection().contains("MODE"));
};

ZEST_CASE(ReadAfterHeldRedefined) {
    // buffer.h reads what config.h, which it includes, defines; the unit
    // includes config.h first and redefines that before buffer.h.
    add_file("shared.hxx", "#pragma once\n#include \"buffer.h\"\n");
    add_file("config.h", "#pragma once\n#define BUF 64\n");
    add_file("buffer.h", R"cpp(#pragma once
#include "config.h"
#if BUF > 100
int big();
#else
int small();
#endif
)cpp");
    add_focus("main.cpp", R"cpp(#include "config.h"
#undef BUF
#define BUF 256
#include "buffer.h"
int use() { return big(); }
)cpp");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("BUF"));
};

ZEST_CASE(IncludedInExternC) {
    add_file("shared.hxx", "#pragma once\n#include \"api.h\"\n");
    add_file("api.h", "#pragma once\nint api(int);\n");
    add_focus("main.cpp", R"cpp(extern "C" {
#include "api.h"
}
int use() { return api(1); }
)cpp");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("api.h"));
    ZEXPECT(!from_pch(*compiled.unit, "api"));
};

ZEST_CASE(IncludedInNamespace) {
    add_file("shared.hxx", "#pragma once\n#include \"api.h\"\n");
    add_file("api.h", "#pragma once\nint api(int);\n");
    add_focus("main.cpp", R"cpp(namespace ns {
#include "api.h"
}
int use() { return ns::api(1); }
)cpp");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("api.h"));
};

ZEST_CASE(IncludedUnderPack) {
    add_file("shared.hxx", "#pragma once\n#include \"api.h\"\n");
    add_file("api.h", "#pragma once\nstruct Packed { char c; int i; };\n");
    add_focus("main.cpp", R"cpp(#pragma pack(push, 1)
#include "api.h"
#pragma pack(pop)
static_assert(sizeof(Packed) == 5);
)cpp");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("api.h"));
};

ZEST_CASE(IncludedAfterANamespace) {
    // The parser leaves the namespace once it reads past the include.
    add_file("shared.hxx", "#pragma once\n#include \"api.h\"\n");
    add_file("api.h", "#pragma once\nint api(int);\n");
    add_focus("main.cpp", R"cpp(namespace detail {
int helper();
}
#include "api.h"
int use() { return api(detail::helper()); }
)cpp");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().empty());
    ZEXPECT(from_pch(*compiled.unit, "api"));
};

ZEST_CASE(IncludedInNamespaceAtEnd) {
    add_file("shared.hxx", "#pragma once\n#include \"api.h\"\n");
    add_file("api.h", "#pragma once\nint api(int);\n");
    add_focus("main.cpp", "namespace ns {\n#include \"api.h\"\n}\n");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("api.h"));
};

ZEST_CASE(IncludedUnderAttribute) {
    add_file("shared.hxx", "#pragma once\n#include \"api.h\"\n");
    add_file("api.h", "#pragma once\nint api(int);\n");
    add_focus(
        "main.cpp",
        R"cpp(#pragma clang attribute push(__attribute__((annotate("tagged"))), apply_to = function)
#include "api.h"
#pragma clang attribute pop
)cpp");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("api.h"));
};

ZEST_CASE(IncludedAfterAttributePop) {
    add_file("shared.hxx", "#pragma once\n#include \"api.h\"\n");
    add_file("api.h", "#pragma once\nint api(int);\n");
    add_focus(
        "main.cpp",
        R"cpp(#pragma clang attribute push(__attribute__((annotate("tagged"))), apply_to = function)
int before();
#pragma clang attribute pop
#include "api.h"
int use() { return api(1); }
)cpp");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().empty());
};

ZEST_CASE(RewrittenAfterHeldHeader) {
    // list.inc, which has no guard, rewrites what the held header it
    // includes defined; table.h reads what list.inc left.
    add_file("shared.hxx", "#pragma once\n#include \"table.h\"\n");
    add_file("count.h", "#pragma once\n#define COUNT 2\n");
    add_file("list.inc", "#include \"count.h\"\n#undef COUNT\n#define COUNT 3\n");
    add_file("table.h", R"cpp(#pragma once
#include "list.inc"
#if COUNT == 3
int three();
#endif
)cpp");
    add_focus("main.cpp", "#include \"table.h\"\nint use() { return three(); }\n");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().empty());
    ZEXPECT(from_pch(*compiled.unit, "three"));
};

ZEST_CASE(NestedInExternC) {
    // The PCH declares api.h where first.h includes it; the unit first
    // includes it through second.h, inside `extern "C"`.
    add_file("shared.hxx", "#pragma once\n#include \"first.h\"\n#include \"second.h\"\n");
    add_file("api.h", "#pragma once\nint api(int);\n");
    add_file("first.h", "#pragma once\n#include \"api.h\"\n");
    add_file("second.h", "#pragma once\nextern \"C\" {\n#include \"api.h\"\n}\n");
    add_focus("main.cpp",
              "#include \"second.h\"\n#include \"first.h\"\nint use() { return api(1); }\n");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("api.h"));
};

ZEST_CASE(CounterInCondition) {
    add_file("shared.hxx", "#pragma once\n#include \"pick.h\"\n");
    add_file("pick.h", R"cpp(#pragma once
#if __COUNTER__ == 0
int first();
#else
int second();
#endif
)cpp");
    add_focus("main.cpp",
              "int unique = __COUNTER__;\n#include \"pick.h\"\nint use() { return second(); }\n");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("pick.h"));
};

ZEST_CASE(LibraryOrder) {
    // Two headers of a library under -isystem: a name of its own is no
    // standard library's business.
    add_file("shared.hxx", "#pragma once\n#include <mode.h>\n#include <api.h>\n");
    add_file("sys/mode.h", "#pragma once\n#define MODE 1\n");
    add_file("sys/api.h", R"cpp(#pragma once
#ifdef MODE
int enabled();
#else
int disabled();
#endif
)cpp");
    add_focus("main.cpp",
              "#include <api.h>\n#include <mode.h>\nint use() { return disabled(); }\n");
    auto compiled = compile_over(*this, {"-isystem", TestVFS::path("sys")});
    ZEXPECT(compiled.unit->shared_rejection().contains("MODE"));
};

ZEST_CASE(ConditionThroughMacro) {
    // FEATURE is no macro where feature.h's condition reads it through
    // ENABLED; the unit defines it.
    add_file("shared.hxx", "#pragma once\n#include \"feature.h\"\n");
    add_file("feature.h", R"cpp(#pragma once
#define ENABLED FEATURE
#if ENABLED
int enabled();
#else
int disabled();
#endif
)cpp");
    add_focus("main.cpp",
              "#define FEATURE 1\n#include \"feature.h\"\nint use() { return enabled(); }\n");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("FEATURE"));
};

ZEST_CASE(IncludedInExternCXX) {
    // `extern "C++"` at the top level is where the PCH declares it too.
    add_file("shared.hxx", "#pragma once\n#include \"api.h\"\n");
    add_file("api.h", "#pragma once\nint api(int);\n");
    add_focus("main.cpp",
              "extern \"C++\" {\n#include \"api.h\"\n}\nint use() { return api(1); }\n");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().empty());
};

ZEST_CASE(OwnExternC) {
    // capi.h gives its functions C linkage itself: an outer `extern "C"`
    // changes nothing.
    add_file("shared.hxx", "#pragma once\n#include \"capi.h\"\n");
    add_file("capi.h", R"cpp(#pragma once
struct handle;
extern "C" {
int capi(struct handle*);
}
)cpp");
    add_focus("main.cpp",
              "extern \"C\" {\n#include \"capi.h\"\n}\nint use() { return capi(nullptr); }\n");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().empty());
    ZEXPECT(from_pch(*compiled.unit, "capi"));
};

ZEST_CASE(PushLeftToIncluder) {
    // push.h pushes MODE for the unit to pop, which the PCH does not keep.
    add_file("shared.hxx", "#pragma once\n#include \"push.h\"\n");
    add_file("push.h", "#pragma once\n#pragma push_macro(\"MODE\")\n");
    add_focus("main.cpp", R"cpp(#define MODE 1
#include "push.h"
#undef MODE
#pragma pop_macro("MODE")
#if MODE
int on();
#endif
)cpp");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.record->macro_stack);
};

ZEST_CASE(PushedInHeldHeader) {
    // mode.h pushes MODE and pops it back: what it restores is what the
    // unit defined, not what the PCH saw.
    add_file("shared.hxx", "#pragma once\n#include \"mode.h\"\n");
    add_file("mode.h", R"cpp(#pragma once
#pragma push_macro("MODE")
#undef MODE
#define MODE 2
int mode();
#pragma pop_macro("MODE")
#if MODE
int on();
#endif
)cpp");
    add_focus("main.cpp", "#define MODE 1\n#include \"mode.h\"\nint use() { return mode(); }\n");
    auto compiled = compile_over(*this);
    ZEXPECT(!compiled.record->macro_stack);
    ZEXPECT(compiled.unit->shared_rejection().contains("MODE"));
};

ZEST_CASE(UnitPushesHeldMacro) {
    // The unit pushes X before config.h defines it: without the PCH, its
    // pop takes X away again.
    add_file("shared.hxx", "#pragma once\n#include \"config.h\"\n");
    add_file("config.h", "#pragma once\n#define X 1\n");
    add_focus("main.cpp", R"cpp(#pragma push_macro("X")
#include "config.h"
#pragma pop_macro("X")
#ifdef X
int x();
#endif
int use() { return 0; }
)cpp");
    auto compiled = compile_over(*this);
    ZEXPECT(compiled.unit->shared_rejection().contains("X"));
};

};  // ZEST_SUITE(SharedHeader)

}  // namespace

}  // namespace clice::testing
