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

TEST_SUITE(Markup) {

TEST_CASE(Escaping) {
    // Check all ASCII punctuation.
    std::string punctuation = R"txt(!"#$%&'()*+,-./:;<=>?@[\]^_`{|}~)txt";
    std::string escaped_punc = R"txt(!"#$%&'()\*+,-./:;<=>?@[\\]^\_\`{|}~)txt";
    ASSERT_EQ(escape(punctuation), escaped_punc);

    // Inline code
    ASSERT_EQ(escape("`foo`"), R"(\`foo\`)");
    ASSERT_EQ(escape("`foo"), R"(\`foo)");
    ASSERT_EQ(escape("foo`"), R"(foo\`)");
    ASSERT_EQ(escape("``foo``"), R"(\`\`foo\`\`)");
    // Code blocks
    ASSERT_EQ(escape("```"), R"(\`\`\`)");  // This could also be inline code!
    ASSERT_EQ(escape("~~~"), R"(\~~~)");

    // Rulers and headings
    ASSERT_TRUE(escaped(escape("## Heading"), '#'));
    ASSERT_TRUE(escaped_none(escape("Foo # bar")));
    ASSERT_EQ(escape("---"), R"(\---)");
    ASSERT_EQ(escape("-"), R"(\-)");
    ASSERT_EQ(escape("==="), R"(\===)");
    ASSERT_EQ(escape("="), R"(\=)");
    ASSERT_EQ(escape("***"), R"(\*\*\*)");  // \** could start emphasis!

    // HTML tags.
    ASSERT_TRUE(escaped(escape("<pre"), '<'));
    ASSERT_TRUE(escaped_none(escape("< pre")));
    ASSERT_TRUE(escaped(escape("if a<b then"), '<'));
    ASSERT_TRUE(escaped_none(escape("if a<b then c.")));
    ASSERT_TRUE(escaped(escape("if a<b then c='foo'."), '<'));
    ASSERT_TRUE(escaped(escape("std::vector<T>"), '<'));
    ASSERT_TRUE(escaped(escape("std::vector<std::string>"), '<'));
    ASSERT_TRUE(escaped_none(escape("std::map<int, int>")));
    // Autolinks
    ASSERT_TRUE(escaped_none(escape("Email <foo@bar.com>")));
    ASSERT_TRUE(escaped_none(escape("Website <http://foo.bar>")));

    // Bullet lists.
    ASSERT_TRUE(escaped(escape("- foo"), '-'));
    ASSERT_TRUE(escaped(escape("* foo"), '*'));
    ASSERT_TRUE(escaped(escape("+ foo"), '+'));
    ASSERT_TRUE(escaped(escape("+"), '+'));
    ASSERT_TRUE(escaped_none(escape("a + foo")));
    ASSERT_TRUE(escaped_none(escape("a+ foo")));
    ASSERT_TRUE(escaped(escape("1. foo"), '.'));
    ASSERT_TRUE(escaped_none(escape("a. foo")));

    // Emphasis.
    ASSERT_EQ(escape("*foo*"), R"(\*foo\*)");
    ASSERT_EQ(escape("**foo**"), R"(\*\*foo\*\*)");
    ASSERT_TRUE(escaped(escape("*foo"), '*'));
    ASSERT_TRUE(escaped_none(escape("foo *")));
    ASSERT_TRUE(escaped_none(escape("foo * bar")));
    ASSERT_TRUE(escaped_none(escape("foo_bar")));
    ASSERT_TRUE(escaped(escape("foo _bar"), '_'));
    ASSERT_TRUE(escaped(escape("foo_ bar"), '_'));
    ASSERT_TRUE(escaped_none(escape("foo _ bar")));

    // HTML entities.
    ASSERT_TRUE(escaped(escape("fish &chips;"), '&'));
    ASSERT_TRUE(escaped_none(escape("fish & chips;")));
    ASSERT_TRUE(escaped_none(escape("fish &chips")));
    ASSERT_TRUE(escaped(escape("foo &#42; bar"), '&'));
    ASSERT_TRUE(escaped(escape("foo &#xaf; bar"), '&'));
    ASSERT_TRUE(escaped_none(escape("foo &?; bar")));

    // Links.
    ASSERT_TRUE(escaped(escape("[foo](bar)"), ']'));
    ASSERT_TRUE(escaped(escape("[foo]: bar"), ']'));
    // No need to escape these, as the target never exists.
    ASSERT_TRUE(escaped_none(escape("[foo][]")));
    ASSERT_TRUE(escaped_none(escape("[foo][bar]")));
    ASSERT_TRUE(escaped_none(escape("[foo]")));

    // In code blocks we don't need to escape ASCII punctuation.
    Paragraph p;
    p.append_code("* foo !+ bar * baz");
    ASSERT_EQ(p.as_markdown(), "`* foo !+ bar * baz`");

    // But we have to escape the backticks.
    p = Paragraph();
    p.append_code("foo`bar`baz", /*preserve=*/true);
    ASSERT_EQ(p.as_markdown(), "`foo``bar``baz`");
    // In plain-text, we fall back to different quotes.
    ASSERT_EQ(p.as_plain_text(), "'foo`bar`baz'");

    // Inline code blocks starting or ending with backticks should add spaces.
    p = Paragraph();
    p.append_code("`foo");
    ASSERT_EQ(p.as_markdown(), "` ``foo `");
    p = Paragraph();
    p.append_code("foo`");
    ASSERT_EQ(p.as_markdown(), "` foo`` `");
    p = Paragraph();
    p.append_code("`foo`");
    ASSERT_EQ(p.as_markdown(), "` ``foo`` `");

    // Code blocks might need more than 3 backticks.
    Document d;
    d.add_code_block("foobarbaz `\nqux");
    ASSERT_EQ(d.as_markdown(), "```cpp\n" "foobarbaz `\nqux\n" "```");
    d = Document();
    d.add_code_block("foobarbaz ``\nqux");
    ASSERT_EQ(d.as_markdown(), "```cpp\n" "foobarbaz ``\nqux\n" "```");
    d = Document();
    d.add_code_block("foobarbaz ```\nqux");
    ASSERT_EQ(d.as_markdown(), "````cpp\n" "foobarbaz ```\nqux\n" "````");
    d = Document();
    d.add_code_block("foobarbaz ` `` ``` ```` `\nqux");
    ASSERT_EQ(d.as_markdown(), "`````cpp\n" "foobarbaz ` `` ``` ```` `\nqux\n" "`````");
}

TEST_CASE(ParagraphChunks) {
    Paragraph p;
    p.append_text("One ");
    p.append_code("fish");
    p.append_text(", two ");
    p.append_code("fish", /*preserve=*/true);

    ASSERT_EQ(p.as_markdown(), "One `fish`, two `fish`");
    ASSERT_EQ(p.as_plain_text(), "One fish, two `fish`");
}

TEST_CASE(ChunkSeparation) {
    // This test keeps appending contents to a single Paragraph and checks
    // expected accumulated contents after each one.
    // Purpose is to check for separation between different chunks.
    Paragraph p;

    p.append_text("after ");
    ASSERT_EQ(p.as_markdown(), "after");
    ASSERT_EQ(p.as_plain_text(), "after");

    p.append_code("foobar").append_space();
    ASSERT_EQ(p.as_markdown(), "after `foobar`");
    ASSERT_EQ(p.as_plain_text(), "after foobar");

    p.append_text("bat");
    ASSERT_EQ(p.as_markdown(), "after `foobar` bat");
    ASSERT_EQ(p.as_plain_text(), "after foobar bat");

    p.append_code("no").append_code("space");
    ASSERT_EQ(p.as_markdown(), "after `foobar` bat`no` `space`");
    ASSERT_EQ(p.as_plain_text(), "after foobar batno space");
}

TEST_CASE(ExtraSpaces) {
    // Make sure spaces inside chunks are dropped.
    Paragraph p;
    p.append_text("foo\n   \t   baz");
    p.append_code(" bar\n");
    ASSERT_EQ(p.as_markdown(), "foo baz`bar`");
    ASSERT_EQ(p.as_plain_text(), "foo bazbar");
}

TEST_CASE(SpacesCollapsed) {
    Paragraph p;
    p.append_text(" foo bar ");
    p.append_text(" baz ");
    ASSERT_EQ(p.as_markdown(), "foo bar baz");
    ASSERT_EQ(p.as_plain_text(), "foo bar baz");
}

TEST_CASE(NewLines) {
    // New lines before and after chunks are dropped.
    Paragraph p;
    p.append_text(" \n foo\nbar\n ");
    p.append_code(" \n foo\nbar \n ");
    ASSERT_EQ(p.as_markdown(), "foo bar `foo bar`");
    ASSERT_EQ(p.as_plain_text(), "foo bar foo bar");
}

TEST_CASE(DocumentSeparators) {
    Document d;
    d.add_paragraph().append_text("foo");
    d.add_code_block("test");
    d.add_paragraph().append_text("bar");

    // Escaped literal: the markdown hard-break "  \n" after "foo" is
    // significant trailing whitespace.
    const char* expected_markdown = "foo  \n" "```cpp\n" "test\n" "```\n" "bar";
    ASSERT_EQ(d.as_markdown(), expected_markdown);

    const char* expected_text = R"pt(foo

test

bar)pt";
    ASSERT_EQ(d.as_plain_text(), expected_text);
}

TEST_CASE(DocumentRuler) {
    Document d;
    d.add_paragraph().append_text("foo");
    d.add_ruler();

    // Ruler followed by paragraph.
    d.add_paragraph().append_text("bar");
    ASSERT_EQ(d.as_markdown(), "foo  \n\n---\nbar");
    ASSERT_EQ(d.as_plain_text(), "foo\n\nbar");

    d = Document();
    d.add_paragraph().append_text("foo");
    d.add_ruler();
    d.add_code_block("bar");
    // Ruler followed by a codeblock.
    ASSERT_EQ(d.as_markdown(), "foo  \n\n---\n```cpp\nbar\n```");
    ASSERT_EQ(d.as_plain_text(), "foo\n\nbar");

    // Ruler followed by another ruler
    d = Document();
    d.add_paragraph().append_text("foo");
    d.add_ruler();
    d.add_ruler();
    ASSERT_EQ(d.as_markdown(), "foo");
    ASSERT_EQ(d.as_plain_text(), "foo");

    // Multiple rulers between blocks
    d.add_ruler();
    d.add_paragraph().append_text("foo");
    ASSERT_EQ(d.as_markdown(), "foo  \n\n---\nfoo");
    ASSERT_EQ(d.as_plain_text(), "foo\n\nfoo");
}

TEST_CASE(DocumentAppend) {
    Document d;
    d.add_paragraph().append_text("foo");
    d.add_ruler();
    Document e;
    e.add_ruler();
    e.add_paragraph().append_text("bar");
    d.append(std::move(e));
    ASSERT_EQ(d.as_markdown(), "foo  \n\n---\nbar");
}

TEST_CASE(DocumentHeading) {
    Document d;
    d.add_heading(1).append_text("foo");
    d.add_heading(2).append_text("bar");
    d.add_paragraph().append_text("baz");
    ASSERT_EQ(d.as_markdown(), "# foo  \n## bar  \nbaz");
    ASSERT_EQ(d.as_plain_text(), "foo\nbar\nbaz");
}

TEST_CASE(CodeBlockRender) {
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
    ASSERT_EQ(d.as_markdown(), expected_markdown);
    ASSERT_EQ(d.as_plain_text(), expected_plain_text);

    d.add_code_block("foo");
    expected_markdown = R"md(```cpp
foo
  bar
  baz
```
```cpp
foo
```)md";
    ASSERT_EQ(d.as_markdown(), expected_markdown);
    expected_plain_text = R"pt(foo
  bar
  baz

foo)pt";
    ASSERT_EQ(d.as_plain_text(), expected_plain_text);
}

TEST_CASE(BulletListRender) {
    BulletList l;
    // Flat list
    l.add_item().add_paragraph().append_text("foo");
    ASSERT_EQ(l.as_markdown(), "- foo");
    ASSERT_EQ(l.as_plain_text(), "- foo");

    l.add_item().add_paragraph().append_text("bar");
    ASSERT_EQ(l.as_markdown(), "- foo\n- bar");
    ASSERT_EQ(l.as_plain_text(), "- foo\n- bar");

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
    const char* expected_markdown = "- foo\n"
                                    "- bar\n"
                                    "- foo  \n"
                                    "  baz  \n"
                                    "  - foo  \n"
                                    "    - baz  \n"
                                    "      baz";
    ASSERT_EQ(l.as_markdown(), expected_markdown);
    const char* expected_plain_text = R"pt(- foo
- bar
- foo
  baz
  - foo
    - baz
      baz)pt";
    ASSERT_EQ(l.as_plain_text(), expected_plain_text);

    // Termination
    inner.add_paragraph().append_text("after");
    // Escaped literal: the list-termination line "    " is whitespace-only.
    expected_markdown = "- foo\n"
                        "- bar\n"
                        "- foo  \n"
                        "  baz  \n"
                        "  - foo  \n"
                        "    - baz  \n"
                        "      baz\n"
                        "    \n"
                        "    after";
    ASSERT_EQ(l.as_markdown(), expected_markdown);
    expected_plain_text = R"pt(- foo
- bar
- foo
  baz
  - foo
    - baz
      baz
    after)pt";
    ASSERT_EQ(l.as_plain_text(), expected_plain_text);
}

};  // TEST_SUITE(Markup)

}  // namespace
}  // namespace clice::testing
