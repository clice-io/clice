#include "Test/Test.h"
#include "Support/Format.h"
#include "Support/StructedText.h"

namespace clice::testing {

namespace {

TEST_SUITE(StructedText) {

TEST_CASE(Paragraph) {
    constexpr const char* cb =
        R"c(// Without processing, recommended
char *longestPalindrome_solv2(const char *s) {
    int len = strlen(s);
    int max_start = 0;
    int max_len = 0;
    for (int i = 0; i < len; ++i) {
        // j = 0, max_len is odd
        // j = 1, max_len is even
        for (int j = 0; j <= 1; ++j) {
            int l = i;
            int r = i + j;

            // expand the range from center
            while (l >= 0 && r < len && s[l] == s[r]) {
                --l;
                ++r;
            }

            ++l;
            --r;

            if (max_len < r - l + 1) {
                max_len = r - l + 1;
                max_start = i;
            }
        }
    }
    char *res = (char *)malloc((max_len + 1) * sizeof(char));
    memcpy(res, s + max_start, max_len);
    res[max_len] = '\0';
    return res;
}
)c";
    StructedText st;
    st.add_paragraph().append_text("CodeBlock Example:").append_newline_char();
    st.add_code_block(cb, "c");
    auto& para = st.add_paragraph();
    para.append_text("para1").append_newline_char();
    /// std::println("{}", st.as_markdown());
}

TEST_CASE(BulletList) {
    StructedText st;
    st.add_bullet_list().add_item().add_paragraph().append_text("Item1");
    st.add_bullet_list().add_item().add_paragraph().append_text("Item2",
                                                                Paragraph::Kind::InlineCode);
    st.add_bullet_list().add_item().add_paragraph().append_text("Item3", Paragraph::Kind::Bold);
    st.add_bullet_list().add_item().add_paragraph().append_text("Item4", Paragraph::Kind::Italic);
    st.add_bullet_list().add_item().add_paragraph().append_text("Item5",
                                                                Paragraph::Kind::Strikethough);
    /// std::println("{}", st.as_markdown());
}

TEST_CASE(FullText) {
    StructedText st;
    st.add_heading(3)
        .append_text("function")
        .append_text("test_bar", Paragraph::Kind::InlineCode)
        .append_newline_char()
        .append_text("Provided by:")
        .append_text("`foo/bar/baz.h`");
    st.add_ruler();
    st.add_paragraph()
        .append_text("→")
        .append_text("int", Paragraph::Kind::InlineCode)
        .append_newline_char();
    st.add_paragraph().append_text("Paramaters:", Paragraph::Kind::Bold).append_newline_char();
    auto& params = st.add_bullet_list();
    params.add_item()
        .add_paragraph()
        .append_text("int foo", Paragraph::Kind::InlineCode)
        .append_text("doc for foo\ndoc for foo line2");
    params.add_item()
        .add_paragraph()
        .append_text("char** bar", Paragraph::Kind::InlineCode)
        .append_text("doc for bar");
    params.add_item()
        .add_paragraph()
        .append_text("char** baz", Paragraph::Kind::InlineCode)
        .append_text("doc for baz");
    st.add_paragraph().append_text(R"md(
brief block
brief line2
    a b c d e f
    ~~~~^
This is *Italic* **Bold** ~~Striketough~~, `InlineCode`
)md");
    st.add_ruler();
    st.add_paragraph().append_text("Details:", Paragraph::Kind::Bold).append_newline_char();
    auto& details = st.add_bullet_list();
    details.add_item().add_paragraph().append_text("Detail1: blah blah...");
    details.add_item().add_paragraph().append_text("Detail2: blah blah...\n    Line2: ......");
    details.add_item().add_paragraph().append_text("Detail3: blah blah...");
    st.add_ruler();
    st.add_paragraph().append_text("Details:", Paragraph::Kind::Bold).append_newline_char();
    auto& warnings = st.add_bullet_list();
    warnings.add_item().add_paragraph().append_text("warnings1: blah blah...");
    warnings.add_item().add_paragraph().append_text("warnings2: blah blah...\n    Line2: ......");
    warnings.add_item().add_paragraph().append_text("warnings3: blah blah...");
    st.add_ruler();
    st.add_code_block("int test_bar(int foo, char **bar, char **baz);\n", "cpp");
    /// std::println("{}", st.as_markdown());
}

};  // TEST_SUITE(StructedText)

}  // namespace

}  // namespace clice::testing
