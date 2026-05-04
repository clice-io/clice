#include "test/test.h"
#include "support/markup.h"

namespace clice::testing {

namespace {

TEST_SUITE(Markup) {

TEST_CASE(EmptyDocument) {
    Markup st;
    EXPECT_EQ(st.as_markdown(), "");
}

TEST_CASE(SingleParagraph) {
    Markup st;
    st.add_paragraph().append_text("hello world");
    EXPECT_EQ(st.as_markdown(), "hello world");
}

TEST_CASE(PlainTextSpacing) {
    Markup st;
    auto& p = st.add_paragraph();
    p.append_text("hello");
    p.append_text("world");
    EXPECT_EQ(st.as_markdown(), "hello world");
}

TEST_CASE(InlineCode) {
    Markup st;
    auto& p = st.add_paragraph();
    p.append_text("Type:");
    p.append_text("int", Paragraph::Kind::InlineCode);
    EXPECT_EQ(st.as_markdown(), "Type: `int`");
}

TEST_CASE(Bold) {
    Markup st;
    st.add_paragraph().append_text("important", Paragraph::Kind::Bold);
    EXPECT_EQ(st.as_markdown(), "**important**");
}

TEST_CASE(Italic) {
    Markup st;
    st.add_paragraph().append_text("emphasis", Paragraph::Kind::Italic);
    EXPECT_EQ(st.as_markdown(), "*emphasis*");
}

TEST_CASE(Strikethrough) {
    Markup st;
    st.add_paragraph().append_text("removed", Paragraph::Kind::Strikethrough);
    EXPECT_EQ(st.as_markdown(), "~~removed~~");
}

TEST_CASE(MixedInline) {
    Markup st;
    auto& p = st.add_paragraph();
    p.append_text("Returns:", Paragraph::Kind::Bold);
    p.append_text("the result");
    EXPECT_EQ(st.as_markdown(), "**Returns:** the result");
}

TEST_CASE(ConsecutiveInlineCode) {
    Markup st;
    auto& p = st.add_paragraph();
    p.append_text("int", Paragraph::Kind::InlineCode);
    p.append_text("x", Paragraph::Kind::InlineCode);
    EXPECT_EQ(st.as_markdown(), "`int` `x`");
}

TEST_CASE(Heading) {
    Markup st;
    st.add_heading(3).append_text("Title");
    EXPECT_EQ(st.as_markdown(), "### Title");
}

TEST_CASE(HeadingWithInlineCode) {
    Markup st;
    auto& h = st.add_heading(2);
    h.append_text("function");
    h.append_text("foo", Paragraph::Kind::InlineCode);
    EXPECT_EQ(st.as_markdown(), "## function `foo`");
}

TEST_CASE(Ruler) {
    Markup st;
    st.add_paragraph().append_text("above");
    st.add_ruler();
    st.add_paragraph().append_text("below");
    auto md = st.as_markdown();
    EXPECT_NE(md.find("above"), std::string::npos);
    EXPECT_NE(md.find("---"), std::string::npos);
    EXPECT_NE(md.find("below"), std::string::npos);
}

TEST_CASE(ConsecutiveRulers) {
    Markup st;
    st.add_paragraph().append_text("text");
    st.add_ruler();
    st.add_ruler();
    st.add_paragraph().append_text("more");
    auto md = st.as_markdown();
    auto first = md.find("---");
    auto second = md.find("---", first + 3);
    EXPECT_EQ(second, std::string::npos);
}

TEST_CASE(LeadingTrailingRulers) {
    Markup st;
    st.add_ruler();
    st.add_paragraph().append_text("content");
    st.add_ruler();
    EXPECT_EQ(st.as_markdown(), "content");
}

TEST_CASE(CodeBlock) {
    Markup st;
    st.add_code_block("int x = 0;", "cpp");
    EXPECT_EQ(st.as_markdown(), "```cpp\nint x = 0;\n```");
}

TEST_CASE(CodeBlockTrailingNewline) {
    Markup st;
    st.add_code_block("int x = 0;\n", "cpp");
    EXPECT_EQ(st.as_markdown(), "```cpp\nint x = 0;\n```");
}

TEST_CASE(CodeBlockNoLang) {
    Markup st;
    st.add_code_block("hello");
    EXPECT_EQ(st.as_markdown(), "```\nhello\n```");
}

TEST_CASE(BulletListSimple) {
    Markup st;
    auto& list = st.add_bullet_list();
    list.add_item().add_paragraph().append_text("one");
    list.add_item().add_paragraph().append_text("two");
    list.add_item().add_paragraph().append_text("three");
    EXPECT_EQ(st.as_markdown(), "- one\n- two\n- three");
}

TEST_CASE(BulletListFormatted) {
    Markup st;
    auto& list = st.add_bullet_list();
    list.add_item().add_paragraph().append_text("bold", Paragraph::Kind::Bold);
    list.add_item().add_paragraph().append_text("code", Paragraph::Kind::InlineCode);
    EXPECT_EQ(st.as_markdown(), "- **bold**\n- `code`");
}

TEST_CASE(BulletListMultiline) {
    Markup st;
    auto& list = st.add_bullet_list();
    list.add_item().add_paragraph().append_text("line1\nline2");
    auto md = st.as_markdown();
    EXPECT_NE(md.find("- line1\n  line2"), std::string::npos);
}

TEST_CASE(BlockSeparation) {
    Markup st;
    st.add_paragraph().append_text("first");
    st.add_paragraph().append_text("second");
    auto md = st.as_markdown();
    EXPECT_NE(md.find("first\n"), std::string::npos);
    EXPECT_NE(md.find("second"), std::string::npos);
    EXPECT_EQ(md.find("firstsecond"), std::string::npos);
}

TEST_CASE(ParagraphThenList) {
    Markup st;
    st.add_paragraph().append_text("Parameters:");
    auto& list = st.add_bullet_list();
    list.add_item().add_paragraph().append_text("int x", Paragraph::Kind::InlineCode);
    auto md = st.as_markdown();
    EXPECT_EQ(md.find("Parameters:- "), std::string::npos);
    EXPECT_EQ(md.find("Parameters:-"), std::string::npos);
    EXPECT_NE(md.find("Parameters:"), std::string::npos);
    EXPECT_NE(md.find("- `int x`"), std::string::npos);
}

TEST_CASE(HeadingThenRuler) {
    Markup st;
    st.add_heading(3).append_text("title");
    st.add_ruler();
    st.add_paragraph().append_text("body");
    auto md = st.as_markdown();
    EXPECT_NE(md.find("### title\n"), std::string::npos);
    EXPECT_NE(md.find("---"), std::string::npos);
    EXPECT_NE(md.find("body"), std::string::npos);
}

TEST_CASE(TripleNewlineCollapse) {
    Markup st;
    st.add_paragraph().append_text("a\n\n\n\nb");
    auto md = st.as_markdown();
    EXPECT_EQ(md.find("\n\n\n"), std::string::npos);
}

TEST_CASE(ClonePreservesHeading) {
    Markup st;
    st.add_heading(2).append_text("Title");
    Markup copy = st;
    auto md = copy.as_markdown();
    EXPECT_NE(md.find("## Title"), std::string::npos);
}

TEST_CASE(NewlineChar) {
    Markup st;
    auto& p = st.add_paragraph();
    p.append_text("line1");
    p.append_newline_char();
    p.append_text("line2");
    auto md = st.as_markdown();
    EXPECT_NE(md.find("line1"), std::string::npos);
    EXPECT_NE(md.find("line2"), std::string::npos);
}

TEST_CASE(FullHoverLike) {
    Markup st;
    st.add_heading(3).append_text("function").append_text("add", Paragraph::Kind::InlineCode);
    st.add_ruler();
    st.add_paragraph().append_text("\xe2\x86\x92").append_text("int", Paragraph::Kind::InlineCode);
    st.add_paragraph().append_text("Parameters:");
    auto& params = st.add_bullet_list();
    params.add_item().add_paragraph().append_text("int a", Paragraph::Kind::InlineCode);
    params.add_item().add_paragraph().append_text("int b", Paragraph::Kind::InlineCode);
    st.add_ruler();
    st.add_code_block("int add(int a, int b);\n", "cpp");

    auto md = st.as_markdown();

    EXPECT_NE(md.find("### function `add`"), std::string::npos);
    EXPECT_NE(md.find("---"), std::string::npos);
    EXPECT_NE(md.find("\xe2\x86\x92 `int`"), std::string::npos);
    EXPECT_NE(md.find("Parameters:"), std::string::npos);
    EXPECT_NE(md.find("- `int a`"), std::string::npos);
    EXPECT_NE(md.find("- `int b`"), std::string::npos);
    EXPECT_NE(md.find("```cpp"), std::string::npos);

    EXPECT_EQ(md.find("`int`Parameters"), std::string::npos);
    EXPECT_EQ(md.find("Parameters:- "), std::string::npos);
}

};  // TEST_SUITE(Markup)

}  // namespace

}  // namespace clice::testing
