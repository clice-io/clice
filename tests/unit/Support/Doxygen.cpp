#include <set>
#include <string>
#include "Test/Test.h"
#include "Support/Doxygen.h"
#include "Support/Format.h"

namespace clice::testing {

namespace {

suite<"Doxygen"> doxygen = [] {
    test("DoxygenInfo") = [&] {
        DoxygenInfo di;
        di.add_param_command_comment("foo",
                                     "Doc for foo",
                                     DoxygenInfo::ParamCommandCommentContent::ParamDirection::In);
        di.add_param_command_comment(
            "bar",
            "Doc for bar",
            DoxygenInfo::ParamCommandCommentContent::ParamDirection::InOut);
        di.add_param_command_comment("baz",
                                     "Doc for baz",
                                     DoxygenInfo::ParamCommandCommentContent::ParamDirection::Out);
        auto param_foo = di.find_param_info("foo");
        expect(param_foo.has_value() && param_foo.value()->content.compare("Doc for foo") == 0);
        auto param_bar = di.find_param_info("bar");
        expect(param_bar.has_value() && param_bar.value()->content.compare("Doc for bar") == 0);
        auto param_non_exists = di.find_param_info("xxx");
        expect(!param_non_exists.has_value());

        for(int i = 0; i < 3; ++i) {
            di.add_block_command_comment("detail", std::format("Detail{}", i));
            di.add_block_command_comment("warning", std::format("Warning{}", i));
            di.add_block_command_comment("note", std::format("Note{}", i));
        }

        std::set<std::string> expected_detail = {"Detail0", "Detail1", "Detail2"};
        std::set<std::string> expected_warning = {"Warning0", "Warning1", "Warning2"};
        std::set<std::string> expected_note = {"Note0", "Note1", "Note2"};

        std::map<std::string, std::set<std::string>> expected{
            {"detail",  std::move(expected_detail) },
            {"warning", std::move(expected_warning)},
            {"note",    std::move(expected_note)   },
        };

        auto bcc_list = di.get_block_command_comments();

        for(auto& [tag, content]: bcc_list) {
            std::set<std::string> actual;
            for(auto& block: content) {
                actual.insert(block.content);
            }
            expect(expected.contains(tag.str()));
            expect(actual == expected[tag.str()]);
            expected.erase(tag.str());
        }

        expect(expected.empty());
    };

    test("DoxygenParserSimple") = [&] {
        // Inline commands
        {
            constexpr auto raw_comment = R"(
 This is a @b Bold word
 This is an \e Italic word
 This is @c InlineCode
)";
            auto [di, md] = strip_doxygen_info(raw_comment);
            expect(di.get_block_command_comments().size() == 0);
            std::println("Rest:\n```{}```", md);
        }

        // ParamCommandComment
        {
            constexpr auto raw_comment = R"( @)";
            std::println("Processing raw comment: `{}`", raw_comment);
            auto [di, md] = strip_doxygen_info(raw_comment);
            std::println("Rest:\n```\n{}\n```\n", md);
        }

        {
            constexpr auto raw_comment = R"( @param)";
            std::println("Processing raw comment: `{}`", raw_comment);
            auto [di, md] = strip_doxygen_info(raw_comment);
            std::println("Rest:\n```\n{}\n```\n", md);
        }

        {
            constexpr auto raw_comment = R"( @param[in,out] foo doc for foo)";
            std::println("Processing raw comment: `{}`", raw_comment);
            auto [di, md] = strip_doxygen_info(raw_comment);
            expect(md.size() == 0);
            auto info_foo = di.find_param_info("foo");
            expect(info_foo.has_value());
            if(info_foo.has_value()) {
                llvm::StringRef doc = info_foo.value()->content;
                expect(info_foo.value()->direction ==
                       DoxygenInfo::ParamCommandCommentContent::ParamDirection::InOut);
                std::println("Doc:\n```\n{}\n```\n", doc);
            }
        }

        {
            constexpr auto raw_comment = R"(
 @param[out] foo doc for foo
 doc for foo line2
 \param[in] bar
 doc for bar

 @param baz
)";
            auto [di, md] = strip_doxygen_info(raw_comment);
            llvm::StringRef rest = md;
            expect(rest.trim().size() == 0);

            auto info_foo = di.find_param_info("foo");
            expect(info_foo.has_value());
            if(info_foo.has_value()) {
                llvm::StringRef doc = info_foo.value()->content;
                expect(info_foo.value()->direction ==
                       DoxygenInfo::ParamCommandCommentContent::ParamDirection::Out);
                std::println("Doc:\n```\n{}\n```", doc);
            }

            auto info_bar = di.find_param_info("bar");
            expect(info_bar.has_value());
            if(info_bar.has_value()) {
                llvm::StringRef doc = info_bar.value()->content;
                expect(info_bar.value()->direction ==
                       DoxygenInfo::ParamCommandCommentContent::ParamDirection::In);
                std::println("Doc:\n```\n{}\n```", doc);
            }

            auto info_baz = di.find_param_info("baz");
            expect(info_baz.has_value());
            if(info_baz.has_value()) {
                llvm::StringRef doc = info_baz.value()->content;
                expect(info_baz.value()->direction ==
                       DoxygenInfo::ParamCommandCommentContent::ParamDirection::Unspecified);
                expect(doc.trim().size() == 0);
            }
        }
    };

    test("DoxygenParserIntegrated") = [&] {
        {
            std::println("##################################################################");
            constexpr auto raw_comment = R"(
 @brief Calculates the area of a rectangle.

 This function computes the area using the formula \c width * height.
 It is considered \b fast and \e reliable.

 @param[in]  width   The width of the rectangle (must be > 0)
 @param[in]  height  The height of the rectangle (must be > 0)
 @return     The area as an integer.

 @note If either width or height is zero, the function returns zero.

 @details
 details 1 blah blah... line1
 details 1 blah blah... line2
        aabbcssss
          ~~~~~~^
 
 A line not in a block
 @details
 details 2 blah blah... line1
 details 2 blah blah... line2
        )";
            auto [di, md] = strip_doxygen_info(raw_comment);
            std::println("Markdown After Stripping:\n```\n{}\n```", md);
            auto info_width = di.find_param_info("width");
            expect(info_width.has_value());
            if(info_width.has_value()) {
                expect(info_width.value()->direction ==
                       DoxygenInfo::ParamCommandCommentContent::ParamDirection::In);
                std::println("Doc for `width`:\n```\n{}\n```", info_width.value()->content);
            }

            auto info_height = di.find_param_info("height");
            expect(info_height.has_value());
            if(info_height.has_value()) {
                expect(info_height.value()->direction ==
                       DoxygenInfo::ParamCommandCommentContent::ParamDirection::In);
                std::println("Doc for `height`:\n```\n{}\n```", info_height.value()->content);
            }

            auto bcc_list = di.get_block_command_comments();
            expect(bcc_list.size() == 3);

            std::println("RegularTags:");
            for(auto& [tag_name, content]: bcc_list) {
                std::println("=================================");
                std::println("Tag name: `{}`", tag_name);
                for(auto& item: content) {
                    std::println("Item:\n```\n{}\n```", item.content);
                }
                std::println("=================================");
            }

            auto ret_info = di.get_return_info();
            expect(ret_info.has_value());
            if(ret_info.has_value()) {
                std::println("Doc for return value:\n```\n{}\n```", ret_info.value());
            }
            std::println("##################################################################");
        }

        // Full test
        {
            constexpr auto raw_comment = R"( @brief brief block
        brief line2

 normal line...
 normal line...
         a b c d e f
         ~~~~^
 normal line...

 @param[in] foo doc for foo
 @param[out] bar doc for bar
            doc for bar line2
 @param[in,out] baz doc for baz
 @param awa not exist. deprecated
 @param foo doc for foo extra line

 @details here are some details
          details line2
  details line3 unproper indent but also detail block

          normal comment line
 @warning watch out
          warn line2

 +------[foo]------+
 |                 |
 |    I'm a box    |
 |                 |
 +-----------------+

 desc line outside
         a b c d e f
         ~~~~^
 This is inline display: @b Bold \e Italic @c InlineCode

 @warning watch out *2

 XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
 YYYYYYYYYYYYYYYYYYYYYYYYYYYYYY
 ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ

 AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA

 BBBBBBBBBBBBBBBBBBBBBBBBBBBBBB

 CCCCCCCCCCCCCCCCCCCCCCCCCCCCCC

 @note here's note1
       note1 line2

 @note here's note2
       note2 line2
       not note2 line3, normal comment

 @return doc for return value
)";
            auto [di, md] = strip_doxygen_info(raw_comment);
            std::println("Markdown After Stripping:\n```\n{}\n```", md);
            auto info_foo = di.find_param_info("foo");
            expect(info_foo.has_value());
            if(info_foo.has_value()) {
                expect(info_foo.value()->direction ==
                       DoxygenInfo::ParamCommandCommentContent::ParamDirection::In);
                std::println("Doc for `foo`:\n```\n{}\n```", info_foo.value()->content);
            }

            auto info_bar = di.find_param_info("bar");
            expect(info_bar.has_value());
            if(info_bar.has_value()) {
                expect(info_bar.value()->direction ==
                       DoxygenInfo::ParamCommandCommentContent::ParamDirection::Out);
                std::println("Doc for `bar`:\n```\n{}\n```", info_bar.value()->content);
            }

            auto info_baz = di.find_param_info("baz");
            expect(info_baz.has_value());
            if(info_baz.has_value()) {
                expect(info_baz.value()->direction ==
                       DoxygenInfo::ParamCommandCommentContent::ParamDirection::InOut);
                std::println("Doc for `baz`:\n```\n{}\n```", info_baz.value()->content);
            }

            auto info_awa = di.find_param_info("awa");
            expect(info_awa.has_value());
            if(info_awa.has_value()) {
                expect(info_awa.value()->direction ==
                       DoxygenInfo::ParamCommandCommentContent::ParamDirection::Unspecified);
                std::println("Doc for `awa`:\n```\n{}\n```", info_awa.value()->content);
            }

            auto bcc_list = di.get_block_command_comments();
            expect(bcc_list.size() == 4);

            std::println("RegularTags:");
            for(auto& [tag_name, content]: bcc_list) {
                std::println("=================================");
                std::println("Tag name: `{}`", tag_name);
                for(auto& item: content) {
                    std::println("Item:\n```\n{}\n```", item.content);
                }
                std::println("=================================");
            }

            auto ret_info = di.get_return_info();
            expect(ret_info.has_value());
            if(ret_info.has_value()) {
                std::println("Doc for return value:\n```\n{}\n```", ret_info.value());
            }
            std::println("##################################################################");
        }
    };
};

}  // namespace

}  // namespace clice::testing
