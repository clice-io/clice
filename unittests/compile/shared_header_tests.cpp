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

    CompilationParams unit{.kind = CompilationKind::Whole, .input = input, .shared = compiled.record};
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

ZEST_CASE(OwnMacroAHeaderReads) {
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

ZEST_CASE(OwnMacroAHeaderSpells) {
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

ZEST_CASE(OwnDefaultOfAHeldMacro) {
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
    // Two system headers read and write a macro only they touch, the unit
    // including them the other way round: accepted.
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
    auto compiled = compile_over(*this, {"-isystem", TestVFS::path("sys")});
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

};  // ZEST_SUITE(SharedHeader)

}  // namespace

}  // namespace clice::testing
