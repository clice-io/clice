module;

#include "modules/prelude.h"

module clice;

import :syntax.documentation;
import :unittests.utils;

namespace clice::testing {
namespace {

Documentation parse(llvm::StringRef text) {
    return parse_documentation(text, [](llvm::StringRef) -> std::optional<Documentation> {
        return std::nullopt;
    });
}

ZEST_SUITE(Documentation) {

ZEST_CASE(DoxygenSections) {
    auto doc = parse(R"(@brief Computes the area.
@param width The horizontal extent.
@param[in] height The vertical extent.
@tparam T The unit.
@return The product.
@retval 0 Nothing to compute.
@throws std::overflow_error On overflow.
@see volume
@deprecated Use area2.)");
    ZEXPECT(doc.brief == "Computes the area.");
    ZASSERT(doc.params.size() == 2U);
    ZEXPECT(doc.params[0].name == "width");
    ZEXPECT(doc.params[0].text == "The horizontal extent.");
    ZEXPECT(doc.params[1].direction == "in");
    ZEXPECT(doc.template_param("T")->text == "The unit.");
    ZEXPECT(doc.returns == "The product.");
    ZASSERT(doc.retvals.size() == 1U);
    ZEXPECT(doc.retvals[0].name == "0");
    ZASSERT(doc.throws.size() == 1U);
    ZEXPECT(doc.throws[0].name == "std::overflow_error");
    ZASSERT(doc.see.size() == 1U);
    ZEXPECT(doc.see[0] == "volume");
    ZASSERT(doc.deprecated.has_value());
    ZEXPECT(*doc.deprecated == "Use area2.");
    ZEXPECT(doc.details == "");
}

ZEST_CASE(OneLineBlock) {
    auto doc = parse("@brief Foo. @param x The x. @return Bar.");
    ZEXPECT(doc.brief == "Foo.");
    ZEXPECT(doc.param("x")->text == "The x.");
    ZEXPECT(doc.returns == "Bar.");
}

ZEST_CASE(SeveralNames) {
    auto doc = parse("\\param[in, out] a,b Both sides.\n          Continued.");
    ZASSERT(doc.params.size() == 2U);
    ZEXPECT(doc.params[1].name == "b");
    ZEXPECT(doc.params[1].direction == "in,out");
    ZEXPECT(doc.params[1].text == "Both sides. Continued.");
}

ZEST_CASE(FirstParagraphBrief) {
    auto doc = parse("Returns the value.\n\nMore details here.");
    ZEXPECT(doc.brief == "Returns the value.");
    ZEXPECT(doc.details == "More details here.");

    auto ended = parse("Computes it.\n@param x The x.");
    ZEXPECT(ended.brief == "Computes it.");

    auto listed = parse("Does X with:\n- a\n- b");
    ZEXPECT(listed.brief == "");
    ZEXPECT(listed.details == "Does X with:\n- a\n- b");
}

ZEST_CASE(MarkdownKept) {
    llvm::StringRef table = "Intro.\n\n| A | B |\n|---|---|\n| 1 | 2 |";
    ZEXPECT(parse(table).details == "| A | B |\n|---|---|\n| 1 | 2 |");

    auto code = parse("Summary.\n\n    step_one();\n    step_two();");
    ZEXPECT(code.details == "    step_one();\n    step_two();");
}

ZEST_CASE(FencedCode) {
    auto doc = parse("Usage:\n\n```cpp\nint x = f(@param);\n```\nAfter.");
    ZEXPECT(doc.params.empty());
    ZEXPECT(doc.details == "```cpp\nint x = f(@param);\n```\nAfter.");
}

ZEST_CASE(CodeCommand) {
    auto doc = parse("Example.\n@code{.cpp}\n  run(1);\n@endcode\n@note Fast.");
    ZEXPECT(doc.details == "```cpp\n  run(1);\n```\n\n**Note:** Fast.");
}

ZEST_CASE(NotCommands) {
    auto doc = parse("Mail user@example.com or see C:\\foo\\bar; @widget stays.");
    ZEXPECT(doc.brief == "Mail user@example.com or see C:\\foo\\bar; @widget stays.");
}

ZEST_CASE(InlineCommands) {
    auto doc =
        parse("Takes \\p count, \\a twice \\b now; use \\c run().\nArea @f$ A = \\pi r^2 @f$.");
    ZEXPECT(doc.brief == "Takes `count`, *twice* **now**; use `run()`. Area `A = \\pi r^2`.");
}

ZEST_CASE(Percent) {
    auto doc = parse("Grows the %vector; prints %d.");
    ZEXPECT(doc.brief == "Grows the vector; prints %d.");
}

ZEST_CASE(LibstdcxxStyle) {
    auto doc = parse(R"(@brief  Add data to the end of the %vector.
@param  __x  Data to be added.

This is a typical stack operation.)");
    ZEXPECT(doc.brief == "Add data to the end of the vector.");
    ZEXPECT(doc.param("__x")->text == "Data to be added.");
    ZEXPECT(doc.details == "This is a typical stack operation.");
}

ZEST_CASE(GtkDoc) {
    auto doc = parse(R"(gtk_widget_show:
@widget: (transfer none): a #GtkWidget

Flags a widget to be displayed.

Returns: %TRUE on success
Since: 2.4)");
    ZEXPECT(doc.param("widget")->text == "a `GtkWidget`");
    ZEXPECT(doc.brief == "Flags a widget to be displayed.");
    ZEXPECT(doc.returns == "`TRUE` on success");
    ZEXPECT(doc.details == "**Since:** 2.4");
}

ZEST_CASE(KernelDoc) {
    auto doc = parse("foo() - Frobnicates the bar.\n@bar: the bar\n\nReturn: zero on success.");
    ZEXPECT(doc.brief == "Frobnicates the bar.");
    ZEXPECT(doc.param("bar")->text == "the bar");
    ZEXPECT(doc.returns == "zero on success.");
}

ZEST_CASE(NotAGtkName) {
    auto doc = parse("Usage:\n\n    run();");
    ZEXPECT(doc.brief == "Usage:");
}

ZEST_CASE(CopyDoc) {
    auto lookup = [](llvm::StringRef name) -> std::optional<Documentation> {
        if(name == "base_func()") {
            return parse("Brief documentation.\n\nDetails.\n@param x The x.\n@return One.");
        }
        return std::nullopt;
    };
    auto doc = parse_documentation("@copydoc base_func()\n@param y The y.", lookup);
    ZEXPECT(doc.brief == "Brief documentation.");
    ZEXPECT(doc.details == "Details.");
    ZEXPECT(doc.param("x")->text == "The x.");
    ZEXPECT(doc.param("y")->text == "The y.");
    ZEXPECT(doc.returns == "One.");

    auto brief = parse_documentation("@copybrief base_func()", lookup);
    ZEXPECT(brief.brief == "Brief documentation.");
    ZEXPECT(brief.details.empty());
    ZEXPECT(brief.params.empty());

    auto details = parse_documentation("Own brief.\n\n@copydetails base_func()", lookup);
    ZEXPECT(details.brief == "Own brief.");
    ZEXPECT(details.details == "Details.");
    ZEXPECT(details.param("x")->text == "The x.");
    ZEXPECT(details.returns == "One.");

    auto missing = parse_documentation("@copydoc other", lookup);
    ZEXPECT(missing.brief == "See `other`.");
}

ZEST_CASE(CopiedNotes) {
    auto lookup = [](llvm::StringRef) -> std::optional<Documentation> {
        return parse("@note Copied.");
    };
    auto doc = parse_documentation("@copydoc other\n\nOwn text.", lookup);
    ZEXPECT(doc.brief.empty());
    ZEXPECT(doc.details == "**Note:** Copied.\n\nOwn text.");
}

ZEST_CASE(CopiedNames) {
    auto plain = parse_copied_name("Widget#draw");
    ZASSERT(plain.has_value());
    ZEXPECT(plain->parts.size() == 2U);
    ZEXPECT(plain->parts[1] == "draw");
    ZEXPECT(!plain->absolute);
    ZEXPECT(!plain->parameters.has_value());

    auto overload = parse_copied_name("::ns::f(const T&, int)");
    ZASSERT(overload.has_value());
    ZEXPECT(overload->absolute);
    ZEXPECT(overload->parts.size() == 2U);
    ZEXPECT(*overload->parameters == "const T&, int");

    ZEXPECT(!parse_copied_name("ns::").has_value());
    ZEXPECT(!parse_copied_name("#f").has_value());
}

ZEST_CASE(DeclaresParameters) {
    llvm::StringRef content = "void f(const std::string &text, int count = 0);";
    auto end = static_cast<std::uint32_t>(content.find('('));
    ZEXPECT(declares_parameters(content, end, "const std::string&, int"));
    ZEXPECT(declares_parameters(content, end, "const std::string & text, int count"));
    ZEXPECT(!declares_parameters(content, end, "const std::string&"));
    ZEXPECT(!declares_parameters(content, end, "const std::string&, int64_t"));

    llvm::StringRef none = "void g(void);";
    ZEXPECT(declares_parameters(none, 6, ""));
    ZEXPECT(!declares_parameters(none, 6, "int"));

    llvm::StringRef templated = "void h<int>(std::map<int, char> m);";
    ZEXPECT(declares_parameters(templated, 6, "std::map<int, char>"));
}

ZEST_CASE(UnclosedBlocks) {
    ZEXPECT(parse("Brief.\n\n```\nrun();").details == "```\nrun();\n```");
    ZEXPECT(parse("Brief.\n\n@code\nrun();").details == "```\nrun();\n```");
}

ZEST_CASE(SectionText) {
    auto doc = parse("Brief.\n\n@section usage Usage\nCall it once.");
    ZEXPECT(doc.details == "**Usage**\n\nCall it once.");
    ZEXPECT(parse("Brief.\n\n@section usage").details == "**usage**");
}

ZEST_CASE(UppercaseMarker) {
    auto doc = parse("Brief.\n\n@TODO: handle NaN.");
    ZEXPECT(doc.params.empty());
    ZEXPECT(doc.details == "@TODO: handle NaN.");
}

ZEST_CASE(NameOnNextLine) {
    auto doc = parse("Brief.\n@throws\n    std::bad_alloc On failure.\n@tparam");
    ZASSERT(doc.throws.size() == 1U);
    ZEXPECT(doc.throws[0].name == "std::bad_alloc");
    ZEXPECT(doc.throws[0].text == "On failure.");
    ZEXPECT(doc.template_params.empty());
}

ZEST_CASE(FenceAfterCommand) {
    auto doc = parse("Formats the value.\n@param x What.\n```\nf(1);\n```");
    ZEXPECT(doc.brief == "Formats the value.");
    ZEXPECT(doc.details == "```\nf(1);\n```");
}

ZEST_CASE(Elsewhere) {
    ZEXPECT(parse("@file\nThe file.").empty());
    ZEXPECT(parse("@{").empty());
    ZEXPECT(parse("@name Accessors\n@{").empty());
}

ZEST_CASE(Notes) {
    auto doc = parse("Brief.\n\n@warning Not thread safe.\n@li one\n@li two");
    ZEXPECT(doc.details == "**Warning:** Not thread safe.\n\n- one\n- two");
}

};  // ZEST_SUITE(Documentation)

}  // namespace
}  // namespace clice::testing
