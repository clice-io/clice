/// Ported from clangd's unittests/support/MarkupTests.cpp (llvmorg-21.1.8), part of the LLVM
/// project, licensed under Apache License v2.0 with LLVM Exceptions.
/// See https://llvm.org/LICENSE.txt for license information.

#include <string>

#include "test/test.h"
#include "support/markup.h"

#include "llvm/ADT/StringRef.h"

namespace clice::testing {

namespace {

using namespace clice::markup;

std::string escape(llvm::StringRef text) {
    return Paragraph().append_text(text).as_markdown();
}

/// Whether \p text contains \p c escaped with a backslash.
bool escaped(llvm::StringRef text, char c) {
    return text.contains(std::string{'\\', c});
}

/// Whether \p text contains no backslash escapes at all.
bool escaped_none(llvm::StringRef text) {
    return !text.contains('\\');
}

ZEST_SUITE(Markup) {

ZEST_CASE(Escaping) {  // Check all ASCII punctuation.
    std::string punctuation = R"txt(!"#$%&'()*+,-./:;<=>?@[\]^_`{|}~)txt";
    std::string escaped_punc = R"txt(!"#$%&'()\*+,-./:;<=>?@[\\]^\_\`{|}~)txt";
    ZASSERT(escape(punctuation) == escaped_punc);

    // Inline code
    ZASSERT(escape("`foo`") == R"(\`foo\`)");
    ZASSERT(escape("`foo") == R"(\`foo)");
    ZASSERT(escape("foo`") == R"(foo\`)");
    ZASSERT(escape("``foo``") == R"(\`\`foo\`\`)");
    // Code blocks
    ZASSERT(escape("```") == R"(\`\`\`)");  // This could also be inline code!
    ZASSERT(escape("~~~") == R"(\~~~)");

    // Rulers and headings
    ZASSERT(escaped(escape("## Heading"), '#'));
    ZASSERT(escaped_none(escape("Foo # bar")));
    ZASSERT(escape("---") == R"(\---)");
    ZASSERT(escape("-") == R"(\-)");
    ZASSERT(escape("===") == R"(\===)");
    ZASSERT(escape("=") == R"(\=)");
    ZASSERT(escape("***") == R"(\*\*\*)");  // \** could start emphasis!

    // HTML tags.
    ZASSERT(escaped(escape("<pre"), '<'));
    ZASSERT(escaped_none(escape("< pre")));
    ZASSERT(escaped(escape("if a<b then"), '<'));
    ZASSERT(escaped_none(escape("if a<b then c.")));
    ZASSERT(escaped(escape("if a<b then c='foo'."), '<'));
    ZASSERT(escaped(escape("std::vector<T>"), '<'));
    ZASSERT(escaped(escape("std::vector<std::string>"), '<'));
    ZASSERT(escaped_none(escape("std::map<int, int>")));
    // Autolinks
    ZASSERT(escaped_none(escape("Email <foo@bar.com>")));
    ZASSERT(escaped_none(escape("Website <http://foo.bar>")));

    // Bullet lists.
    ZASSERT(escaped(escape("- foo"), '-'));
    ZASSERT(escaped(escape("* foo"), '*'));
    ZASSERT(escaped(escape("+ foo"), '+'));
    ZASSERT(escaped(escape("+"), '+'));
    ZASSERT(escaped_none(escape("a + foo")));
    ZASSERT(escaped_none(escape("a+ foo")));
    ZASSERT(escaped(escape("1. foo"), '.'));
    ZASSERT(escaped_none(escape("a. foo")));

    // Emphasis.
    ZASSERT(escape("*foo*") == R"(\*foo\*)");
    ZASSERT(escape("**foo**") == R"(\*\*foo\*\*)");
    ZASSERT(escaped(escape("*foo"), '*'));
    ZASSERT(escaped_none(escape("foo *")));
    ZASSERT(escaped_none(escape("foo * bar")));
    ZASSERT(escaped_none(escape("foo_bar")));
    ZASSERT(escaped(escape("foo _bar"), '_'));
    ZASSERT(escaped(escape("foo_ bar"), '_'));
    ZASSERT(escaped_none(escape("foo _ bar")));

    // HTML entities.
    ZASSERT(escaped(escape("fish &chips;"), '&'));
    ZASSERT(escaped_none(escape("fish & chips;")));
    ZASSERT(escaped_none(escape("fish &chips")));
    ZASSERT(escaped(escape("foo &#42; bar"), '&'));
    ZASSERT(escaped(escape("foo &#xaf; bar"), '&'));
    ZASSERT(escaped_none(escape("foo &?; bar")));

    // Links.
    ZASSERT(escaped(escape("[foo](bar)"), ']'));
    ZASSERT(escaped(escape("[foo]: bar"), ']'));
    // No need to escape these, as the target never exists.
    ZASSERT(escaped_none(escape("[foo][]")));
    ZASSERT(escaped_none(escape("[foo][bar]")));
    ZASSERT(escaped_none(escape("[foo]")));

    // In code blocks we don't need to escape ASCII punctuation.
    Paragraph p;
    p.append_code("* foo !+ bar * baz");
    ZASSERT(p.as_markdown() == "`* foo !+ bar * baz`");

    // But we have to escape the backticks.
    p = Paragraph();
    p.append_code("foo`bar`baz", /*preserve=*/true);
    ZASSERT(p.as_markdown() == "`foo``bar``baz`");
    // In plain-text, we fall back to different quotes.
    ZASSERT(p.as_plain_text() == "'foo`bar`baz'");

    // Inline code blocks starting or ending with backticks should add spaces.
    p = Paragraph();
    p.append_code("`foo");
    ZASSERT(p.as_markdown() == "` ``foo `");
    p = Paragraph();
    p.append_code("foo`");
    ZASSERT(p.as_markdown() == "` foo`` `");
    p = Paragraph();
    p.append_code("`foo`");
    ZASSERT(p.as_markdown() == "` ``foo`` `");

    // Code blocks might need more than 3 backticks.
    Document d;
    d.add_code_block("foobarbaz `\nqux");
    ZASSERT(d.as_markdown() ==
            "```cpp\n"
            "foobarbaz `\nqux\n"
            "```");
    d = Document();
    d.add_code_block("foobarbaz ``\nqux");
    ZASSERT(d.as_markdown() ==
            "```cpp\n"
            "foobarbaz ``\nqux\n"
            "```");
    d = Document();
    d.add_code_block("foobarbaz ```\nqux");
    ZASSERT(d.as_markdown() ==
            "````cpp\n"
            "foobarbaz ```\nqux\n"
            "````");
    d = Document();
    d.add_code_block("foobarbaz ` `` ``` ```` `\nqux");
    ZASSERT(d.as_markdown() ==
            "`````cpp\n"
            "foobarbaz ` `` ``` ```` `\nqux\n"
            "`````");
}

ZEST_CASE(ParagraphChunks) {
    Paragraph p;
    p.append_text("One ");
    p.append_code("fish");
    p.append_text(", two ");
    p.append_code("fish", /*preserve=*/true);

    ZASSERT(p.as_markdown() == "One `fish`, two `fish`");
    ZASSERT(p.as_plain_text() == "One fish, two `fish`");
}

ZEST_CASE(ChunkSeparation) {
    // This test keeps appending contents to a single Paragraph and checks
    // expected accumulated contents after each one.
    // Purpose is to check for separation between different chunks.
    Paragraph p;

    p.append_text("after ");
    ZASSERT(p.as_markdown() == "after");
    ZASSERT(p.as_plain_text() == "after");

    p.append_code("foobar").append_space();
    ZASSERT(p.as_markdown() == "after `foobar`");
    ZASSERT(p.as_plain_text() == "after foobar");

    p.append_text("bat");
    ZASSERT(p.as_markdown() == "after `foobar` bat");
    ZASSERT(p.as_plain_text() == "after foobar bat");

    p.append_code("no").append_code("space");
    ZASSERT(p.as_markdown() == "after `foobar` bat`no` `space`");
    ZASSERT(p.as_plain_text() == "after foobar batno space");
}

ZEST_CASE(ExtraSpaces) {
    // Make sure spaces inside chunks are dropped.
    Paragraph p;
    p.append_text("foo\n   \t   baz");
    p.append_code(" bar\n");
    ZASSERT(p.as_markdown() == "foo baz`bar`");
    ZASSERT(p.as_plain_text() == "foo bazbar");
}

ZEST_CASE(SpacesCollapsed) {
    Paragraph p;
    p.append_text(" foo bar ");
    p.append_text(" baz ");
    ZASSERT(p.as_markdown() == "foo bar baz");
    ZASSERT(p.as_plain_text() == "foo bar baz");
}

ZEST_CASE(NewLines) {
    // New lines before and after chunks are dropped.
    Paragraph p;
    p.append_text(" \n foo\nbar\n ");
    p.append_code(" \n foo\nbar \n ");
    ZASSERT(p.as_markdown() == "foo bar `foo bar`");
    ZASSERT(p.as_plain_text() == "foo bar foo bar");
}

ZEST_CASE(DocumentSeparators) {
    Document d;
    d.add_paragraph().append_text("foo");
    d.add_code_block("test");
    d.add_paragraph().append_text("bar");

    // Escaped literal: the markdown hard-break "  \n" after "foo" is
    // significant trailing whitespace.
    const char* expected_markdown =
        "foo  \n"
        "```cpp\n"
        "test\n"
        "```\n"
        "bar";
    ZASSERT(d.as_markdown() == expected_markdown);

    const char* expected_text = R"pt(foo

test

bar)pt";
    ZASSERT(d.as_plain_text() == expected_text);
}

ZEST_CASE(DocumentRuler) {
    Document d;
    d.add_paragraph().append_text("foo");
    d.add_ruler();

    // Ruler followed by paragraph.
    d.add_paragraph().append_text("bar");
    ZASSERT(d.as_markdown() == "foo  \n\n---\nbar");
    ZASSERT(d.as_plain_text() == "foo\n\nbar");

    d = Document();
    d.add_paragraph().append_text("foo");
    d.add_ruler();
    d.add_code_block("bar");
    // Ruler followed by a codeblock.
    ZASSERT(d.as_markdown() == "foo  \n\n---\n```cpp\nbar\n```");
    ZASSERT(d.as_plain_text() == "foo\n\nbar");

    // Ruler followed by another ruler
    d = Document();
    d.add_paragraph().append_text("foo");
    d.add_ruler();
    d.add_ruler();
    ZASSERT(d.as_markdown() == "foo");
    ZASSERT(d.as_plain_text() == "foo");

    // Multiple rulers between blocks
    d.add_ruler();
    d.add_paragraph().append_text("foo");
    ZASSERT(d.as_markdown() == "foo  \n\n---\nfoo");
    ZASSERT(d.as_plain_text() == "foo\n\nfoo");
}

ZEST_CASE(DocumentAppend) {
    Document d;
    d.add_paragraph().append_text("foo");
    d.add_ruler();
    Document e;
    e.add_ruler();
    e.add_paragraph().append_text("bar");
    d.append(std::move(e));
    ZASSERT(d.as_markdown() == "foo  \n\n---\nbar");
}

ZEST_CASE(DocumentHeading) {
    Document d;
    d.add_heading(1).append_text("foo");
    d.add_heading(2).append_text("bar");
    d.add_paragraph().append_text("baz");
    ZASSERT(d.as_markdown() == "# foo  \n## bar  \nbaz");
    ZASSERT(d.as_plain_text() == "foo\nbar\nbaz");
}

ZEST_CASE(CodeBlockRender) {
    Document d;
    // Code blocks preserves any extra spaces.
    d.add_code_block("foo\n  bar\n  baz");

    llvm::StringRef expected_markdown = R"md(```cpp
foo
  bar
  baz
```)md";
    llvm::StringRef expected_plain_text = R"pt(foo
  bar
  baz)pt";
    ZASSERT(d.as_markdown() == expected_markdown);
    ZASSERT(d.as_plain_text() == expected_plain_text);

    d.add_code_block("foo");
    expected_markdown = R"md(```cpp
foo
  bar
  baz
```
```cpp
foo
```)md";
    ZASSERT(d.as_markdown() == expected_markdown);
    expected_plain_text = R"pt(foo
  bar
  baz

foo)pt";
    ZASSERT(d.as_plain_text() == expected_plain_text);
}

ZEST_CASE(BulletListRender) {
    BulletList l;
    // Flat list
    l.add_item().add_paragraph().append_text("foo");
    ZASSERT(l.as_markdown() == "- foo");
    ZASSERT(l.as_plain_text() == "- foo");

    l.add_item().add_paragraph().append_text("bar");
    ZASSERT(l.as_markdown() == "- foo\n- bar");
    ZASSERT(l.as_plain_text() == "- foo\n- bar");

    // Nested list, with a single item.
    Document& d = l.add_item();
    // First item with foo\nbaz
    d.add_paragraph().append_text("foo");
    d.add_paragraph().append_text("baz");

    // Nest one level.
    Document& inner = d.add_bullet_list().add_item();
    inner.add_paragraph().append_text("foo");

    // Nest one more level.
    BulletList& inner_list = inner.add_bullet_list();
    // Single item, baz\nbaz
    Document& deep_doc = inner_list.add_item();
    deep_doc.add_paragraph().append_text("baz");
    deep_doc.add_paragraph().append_text("baz");

    // Escaped literals: markdown hard-breaks "  \n" are significant trailing
    // whitespace.
    const char* expected_markdown =
        "- foo\n"
        "- bar\n"
        "- foo  \n"
        "  baz  \n"
        "  - foo  \n"
        "    - baz  \n"
        "      baz";
    ZASSERT(l.as_markdown() == expected_markdown);
    const char* expected_plain_text = R"pt(- foo
- bar
- foo
  baz
  - foo
    - baz
      baz)pt";
    ZASSERT(l.as_plain_text() == expected_plain_text);

    // Termination
    inner.add_paragraph().append_text("after");
    // Escaped literal: the list-termination line "    " is whitespace-only.
    expected_markdown =
        "- foo\n"
        "- bar\n"
        "- foo  \n"
        "  baz  \n"
        "  - foo  \n"
        "    - baz  \n"
        "      baz\n"
        "    \n"
        "    after";
    ZASSERT(l.as_markdown() == expected_markdown);
    expected_plain_text = R"pt(- foo
- bar
- foo
  baz
  - foo
    - baz
      baz
    after)pt";
    ZASSERT(l.as_plain_text() == expected_plain_text);
}

};  // ZEST_SUITE(Markup)

}  // namespace
}  // namespace clice::testing
