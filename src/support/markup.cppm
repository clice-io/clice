module;

#include "modules/prelude.h"
/// Ported from clangd's support/Markup.h (llvmorg-21.1.8), part of the LLVM
/// project, licensed under Apache License v2.0 with LLVM Exceptions.
/// See https://llvm.org/LICENSE.txt for license information.

module clice:support.markup;

/// A model of formatted text that can be rendered to plaintext or markdown.
namespace clice::markup {

/// Wrap `text` in a code span, with enough backticks for the ones it
/// holds.
std::string code_span(llvm::StringRef text);

/// Holds text and knows how to lay it out. Multiple blocks can be grouped to
/// form a document. Blocks include their own trailing newlines, container
/// should trim them if need be.
class Block {
public:
    virtual void render_markdown(llvm::raw_ostream& os) const = 0;
    virtual void render_plain_text(llvm::raw_ostream& os) const = 0;

    std::string as_markdown() const;
    std::string as_plain_text() const;

    virtual bool is_ruler() const {
        return false;
    }

    virtual ~Block() = default;
};

/// Represents parts of the markup that can contain strings, like inline code,
/// code block or plain text.
/// One must introduce different paragraphs to create separate blocks.
class Paragraph : public Block {
public:
    void render_markdown(llvm::raw_ostream& os) const override;
    void render_plain_text(llvm::raw_ostream& os) const override;

    /// Append plain text to the end of the string.
    Paragraph& append_text(llvm::StringRef text);

    /// Append inline code, this translates to the ` block in markdown.
    /// \p preserve indicates the code span must be apparent even in plaintext.
    Paragraph& append_code(llvm::StringRef code, bool preserve = false);

    /// Append text written in markdown, kept as it is (see
    /// Document::add_markdown).
    Paragraph& append_markdown(llvm::StringRef text);

    /// Append plain text shown in bold where markup shows.
    Paragraph& append_strong(llvm::StringRef text);

    /// Ensure there is space between the surrounding chunks.
    /// Has no effect at the beginning or end of a paragraph.
    Paragraph& append_space();

private:
    struct Chunk {
        enum Kind : uint8_t {
            PlainText,
            InlineCode,
            Markdown,
            Strong,
        } kind = PlainText;

        /// Preserve chunk markers in plaintext.
        bool preserve = false;

        std::string contents;

        /// Whether this chunk should be surrounded by whitespace.
        /// Consecutive space_after and space_before will be collapsed into one
        /// space. Code spans don't usually set this: their spaces belong
        /// "inside" the span.
        bool space_before = false;
        bool space_after = false;
    };

    std::vector<Chunk> chunks;
};

class Document;

/// Represents a sequence of one or more documents. Knows how to print them in
/// a list like format, e.g. by prepending with "- " and indentation.
class BulletList : public Block {
public:
    BulletList();
    ~BulletList();

    void render_markdown(llvm::raw_ostream& os) const override;
    void render_plain_text(llvm::raw_ostream& os) const override;

    Document& add_item();

private:
    std::vector<Document> items;
};

/// A format-agnostic representation for structured text. Allows rendering into
/// markdown and plaintext.
class Document {
public:
    /// Adds a semantical block that will be separate from others.
    Paragraph& add_paragraph();

    /// Inserts a horizontal separator to the document.
    void add_ruler();

    /// Inserts an empty line between the blocks around it.
    void add_spacing();

    /// Adds a block of code. This translates to a ``` block in markdown. In
    /// plain text representation, the code block will be surrounded by
    /// newlines.
    void add_code_block(std::string code, std::string language = "cpp");

    /// Heading is a special type of paragraph that will be prepended with
    /// \p level many '#'s in markdown.
    Paragraph& add_heading(std::size_t level);

    BulletList& add_bullet_list();

    /// Adds text written in markdown, kept as it is: in markdown with only
    /// the `<` escaped that would start an HTML tag where none is meant
    /// (`vector<int>`), in plain text verbatim.
    void add_markdown(std::string text);

    /// Adds text to show as written, line by line.
    void add_lines(std::string text);

    /// Doesn't contain any trailing newlines.
    /// We try to make the markdown human-readable, e.g. avoid extra escaping.
    /// At least one client (coc.nvim) displays the markdown verbatim!
    std::string as_markdown() const;

    /// Doesn't contain any trailing newlines.
    std::string as_plain_text() const;

private:
    std::vector<std::unique_ptr<Block>> children;
};

}  // namespace clice::markup
