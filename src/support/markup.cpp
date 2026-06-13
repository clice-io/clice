/// Ported from clangd's support/Markup.cpp (llvmorg-21.1.8), part of the LLVM
/// project, licensed under Apache License v2.0 with LLVM Exceptions.
/// See https://llvm.org/LICENSE.txt for license information.

#include "support/markup.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::markup {

namespace {

/// Is <contents a plausible start to an HTML tag?
/// Contents may not be the rest of the line, but it's the rest of the plain
/// text, so we expect to see at least the tag name.
bool looks_like_tag(llvm::StringRef contents) {
    if(contents.empty()) {
        return false;
    }

    if(contents.front() == '!' || contents.front() == '?' || contents.front() == '/') {
        return true;
    }

    // Check the start of the tag name.
    if(!llvm::isAlpha(contents.front())) {
        return false;
    }

    // Drop rest of the tag name, and following whitespace.
    contents =
        contents
            .drop_while([](char c) { return llvm::isAlnum(c) || c == '-' || c == '_' || c == ':'; })
            .drop_while(llvm::isSpace);

    // The rest of the tag consists of attributes, which have restrictive names.
    // If we hit '=', all bets are off (attribute values can contain anything).
    for(; !contents.empty(); contents = contents.drop_front()) {
        if(llvm::isAlnum(contents.front()) || llvm::isSpace(contents.front())) {
            continue;
        }

        if(contents.front() == '>' || contents.starts_with("/>")) {
            return true;  // May close the tag.
        }

        if(contents.front() == '=') {
            return true;  // Don't try to parse attribute values.
        }

        return false;  // Random punctuation means this isn't a tag.
    }

    return true;  // Potentially incomplete tag.
}

/// Tests whether \p c should be backslash-escaped in markdown.
/// The string being escaped is `before + c + after`. This is part of a
/// paragraph. \p starts_line indicates whether \p before is the start of the
/// line. \p after may not be everything until the end of the line.
///
/// It's always safe to escape punctuation, but want minimal escaping.
/// The strategy is to escape the first character of anything that might start
/// a markdown grammar construct.
bool needs_leading_escape(char c, llvm::StringRef before, llvm::StringRef after, bool starts_line) {
    assert(before.take_while(llvm::isSpace).empty());

    auto ruler_length = [&]() -> unsigned {
        if(!starts_line || !before.empty()) {
            return 0;
        }
        llvm::StringRef rest = after.rtrim();
        return std::ranges::all_of(rest, [c](char d) { return c == d; }) ? 1 + rest.size() : 0;
    };

    auto is_bullet = [&]() {
        return starts_line && before.empty() && (after.empty() || after.starts_with(" "));
    };

    auto space_surrounds = [&]() {
        return (after.empty() || llvm::isSpace(after.front())) &&
               (before.empty() || llvm::isSpace(before.back()));
    };

    auto word_surrounds = [&]() {
        return (!after.empty() && llvm::isAlnum(after.front())) &&
               (!before.empty() && llvm::isAlnum(before.back()));
    };

    switch(c) {
        case '\\':  // Escaped character.
            return true;
        case '`':  // Code block or inline code.
            // Any number of backticks can delimit an inline code block that
            // can end anywhere (including on another line). We must escape
            // them all.
            return true;
        case '~':  // Code block.
            return starts_line && before.empty() && after.starts_with("~~");
        case '#': {  // ATX heading.
            if(!starts_line || !before.empty()) {
                return false;
            }
            llvm::StringRef rest = after.ltrim(c);
            return rest.empty() || rest.starts_with(" ");
        }
        case ']':  // Link or link reference.
            // We escape ] rather than [ here, because it's more constrained:
            //   ](...) is an in-line link
            //   ]: is a link reference
            // The following are only links if the link reference exists:
            //   ] by itself is a shortcut link
            //   ][...] is an out-of-line link
            // Because we never emit link references, we don't need to handle
            // these.
            return after.starts_with(":") || after.starts_with("(");
        case '=':  // Setex heading.
            return ruler_length() > 0;
        case '_':  // Horizontal ruler or matched delimiter.
            if(ruler_length() >= 3) {
                return true;
            }
            // Not a delimiter if surrounded by space, or inside a word.
            // (The rules at word boundaries are subtle).
            return !(space_surrounds() || word_surrounds());
        case '-':  // Setex heading, horizontal ruler, or bullet.
            if(ruler_length() > 0) {
                return true;
            }
            return is_bullet();
        case '+':  // Bullet list.
            return is_bullet();
        case '*':  // Bullet list, horizontal ruler, or delimiter.
            return is_bullet() || ruler_length() >= 3 || !space_surrounds();
        case '<':  // HTML tag (or autolink, which we choose not to escape).
            return looks_like_tag(after);
        case '>':  // Quote marker. Needs escaping at start of line.
            return starts_line && before.empty();
        case '&': {  // HTML entity reference.
            auto end = after.find(';');
            if(end == llvm::StringRef::npos) {
                return false;
            }
            llvm::StringRef content = after.substr(0, end);
            if(content.consume_front("#")) {
                if(content.consume_front("x") || content.consume_front("X")) {
                    return std::ranges::all_of(content, llvm::isHexDigit);
                }
                return std::ranges::all_of(content, llvm::isDigit);
            }
            return std::ranges::all_of(content, llvm::isAlpha);
        }
        case '.':  // Numbered list indicator. Escape 12. -> 12\. at start of line.
        case ')':
            return starts_line && !before.empty() && std::ranges::all_of(before, llvm::isDigit) &&
                   after.starts_with(" ");
        default: return false;
    }
}

/// Escape a markdown text block. Ensures the punctuation will not introduce
/// any of the markdown constructs.
std::string render_text(llvm::StringRef input, bool starts_line) {
    std::string result;
    for(unsigned i = 0; i < input.size(); ++i) {
        if(needs_leading_escape(input[i], input.substr(0, i), input.substr(i + 1), starts_line)) {
            result.push_back('\\');
        }
        result.push_back(input[i]);
    }
    return result;
}

/// Renders \p input as an inline block of code in markdown. The returned value
/// is surrounded by backticks and the inner contents are properly escaped.
std::string render_inline_block(llvm::StringRef input) {
    std::string result;
    // Double all backticks to make sure we don't close the inline block early.
    for(std::size_t from = 0; from < input.size();) {
        std::size_t next = input.find("`", from);
        result += input.substr(from, next - from);
        if(next == llvm::StringRef::npos) {
            break;
        }
        result += "``";  // Double the found backtick.

        from = next + 1;
    }

    // If results starts with a backtick, add spaces on both sides. The spaces
    // are ignored by markdown renderers.
    if(llvm::StringRef(result).starts_with("`") || llvm::StringRef(result).ends_with("`")) {
        return "` " + std::move(result) + " `";
    }

    // Markdown render should ignore first and last space if both are there. We
    // add an extra pair of spaces in that case to make sure we render what the
    // user intended.
    if(llvm::StringRef(result).starts_with(" ") && llvm::StringRef(result).ends_with(" ")) {
        return "` " + std::move(result) + " `";
    }

    return "`" + std::move(result) + "`";
}

/// Get marker required for \p input to represent a markdown codeblock. It
/// consists of at least 3 backticks(`). Although markdown also allows to use
/// tilde(~) for code blocks, they are never used.
std::string get_marker_for_code_block(llvm::StringRef input) {
    // Count the maximum number of consecutive backticks in \p input. We need
    // to start and end the code block with more.
    unsigned max_backticks = 0;
    unsigned backticks = 0;
    for(char c: input) {
        if(c == '`') {
            ++backticks;
            continue;
        }
        max_backticks = std::max(max_backticks, backticks);
        backticks = 0;
    }
    max_backticks = std::max(backticks, max_backticks);

    // Use the corresponding number of backticks to start and end a code block.
    return std::string(std::max(3u, max_backticks + 1), '`');
}

/// Trims the input and concatenates whitespace blocks into a single ` `.
std::string canonicalize_spaces(llvm::StringRef input) {
    llvm::SmallVector<llvm::StringRef> words;
    llvm::SplitString(input, words);
    return llvm::join(words, " ");
}

std::string render_blocks(llvm::ArrayRef<std::unique_ptr<Block>> children,
                          void (Block::*render_func)(llvm::raw_ostream&) const) {
    std::string text;
    llvm::raw_string_ostream os(text);

    // Trim rulers.
    children = children.drop_while([](const std::unique_ptr<Block>& c) { return c->is_ruler(); });
    while(!children.empty() && children.back()->is_ruler()) {
        children = children.drop_back();
    }

    bool last_block_was_ruler = true;
    for(const auto& c: children) {
        if(c->is_ruler() && last_block_was_ruler) {
            continue;
        }
        last_block_was_ruler = c->is_ruler();
        ((*c).*render_func)(os);
    }

    // Get rid of redundant empty lines introduced in plaintext while imitating
    // padding in markdown.
    std::string adjusted;
    llvm::StringRef trimmed = llvm::StringRef(text).trim();
    adjusted.reserve(trimmed.size());
    for(char c: trimmed) {
        // We allow at most two consecutive newlines.
        if(c == '\n' && llvm::StringRef(adjusted).ends_with("\n\n")) {
            continue;
        }
        adjusted.push_back(c);
    }

    return adjusted;
}

/// Separates two blocks with extra spacing. Note that it might render
/// strangely in vscode if the trailing block is a codeblock, see
/// https://github.com/microsoft/vscode/issues/88416 for details.
class Ruler : public Block {
public:
    void render_markdown(llvm::raw_ostream& os) const override {
        // Note that we need an extra new line before the ruler, otherwise we
        // might make previous block a title instead of introducing a ruler.
        os << "\n---\n";
    }

    void render_plain_text(llvm::raw_ostream& os) const override {
        os << '\n';
    }

    std::unique_ptr<Block> clone() const override {
        return std::make_unique<Ruler>(*this);
    }

    bool is_ruler() const override {
        return true;
    }
};

class CodeBlock : public Block {
public:
    CodeBlock(std::string contents, std::string language) :
        contents(std::move(contents)), language(std::move(language)) {}

    void render_markdown(llvm::raw_ostream& os) const override {
        std::string marker = get_marker_for_code_block(contents);
        // No need to pad from previous blocks, as they should end with a new
        // line.
        os << marker << language << '\n' << contents << '\n' << marker << '\n';
    }

    void render_plain_text(llvm::raw_ostream& os) const override {
        // In plaintext we want one empty line before and after codeblocks.
        os << '\n' << contents << "\n\n";
    }

    std::unique_ptr<Block> clone() const override {
        return std::make_unique<CodeBlock>(*this);
    }

private:
    std::string contents;
    std::string language;
};

/// Inserts two spaces after each `\n` to indent each line. First line is not
/// indented.
std::string indent_lines(llvm::StringRef input) {
    assert(!input.ends_with("\n") && "Input should've been trimmed.");
    std::string indented;
    // We'll add 2 spaces after each new line.
    indented.reserve(input.size() + input.count('\n') * 2);
    for(char c: input) {
        indented += c;
        if(c == '\n') {
            indented.append("  ");
        }
    }
    return indented;
}

class Heading : public Paragraph {
public:
    Heading(std::size_t level) : level(level) {}

    void render_markdown(llvm::raw_ostream& os) const override {
        os << std::string(level, '#') << ' ';
        Paragraph::render_markdown(os);
    }

    /// Paragraph::clone would slice off the heading level.
    std::unique_ptr<Block> clone() const override {
        return std::make_unique<Heading>(*this);
    }

private:
    std::size_t level;
};

/// Choose a marker to delimit \p text from a prioritized list of options.
/// This is more readable than escaping for plain-text.
llvm::StringRef choose_marker(llvm::ArrayRef<llvm::StringRef> options, llvm::StringRef text) {
    // Prefer a delimiter whose characters don't appear in the text.
    for(llvm::StringRef option: options) {
        if(text.find_first_of(option) == llvm::StringRef::npos) {
            return option;
        }
    }
    return options.front();
}

}  // namespace

std::string Block::as_markdown() const {
    std::string result;
    llvm::raw_string_ostream os(result);
    render_markdown(os);
    return llvm::StringRef(result).trim().str();
}

std::string Block::as_plain_text() const {
    std::string result;
    llvm::raw_string_ostream os(result);
    render_plain_text(os);
    return llvm::StringRef(result).trim().str();
}

void Paragraph::render_markdown(llvm::raw_ostream& os) const {
    bool needs_space = false;
    bool has_chunks = false;
    for(auto& chunk: chunks) {
        if(chunk.space_before || needs_space) {
            os << " ";
        }

        switch(chunk.kind) {
            case Chunk::PlainText: os << render_text(chunk.contents, !has_chunks); break;
            case Chunk::InlineCode: os << render_inline_block(chunk.contents); break;
        }

        has_chunks = true;
        needs_space = chunk.space_after;
    }
    // Paragraphs are translated into markdown lines, not markdown paragraphs.
    // Therefore it only has a single linebreak afterwards.
    // VSCode requires two spaces at the end of line to start a new one.
    os << "  \n";
}

void Paragraph::render_plain_text(llvm::raw_ostream& os) const {
    bool needs_space = false;
    for(auto& chunk: chunks) {
        if(chunk.space_before || needs_space) {
            os << " ";
        }

        llvm::StringRef marker = "";
        if(chunk.preserve && chunk.kind == Chunk::InlineCode) {
            marker = choose_marker({"`", "'", "\""}, chunk.contents);
        }
        os << marker << chunk.contents << marker;

        needs_space = chunk.space_after;
    }
    os << '\n';
}

std::unique_ptr<Block> Paragraph::clone() const {
    return std::make_unique<Paragraph>(*this);
}

Paragraph& Paragraph::append_space() {
    if(!chunks.empty()) {
        chunks.back().space_after = true;
    }
    return *this;
}

Paragraph& Paragraph::append_text(llvm::StringRef text) {
    std::string norm = canonicalize_spaces(text);
    if(norm.empty()) {
        return *this;
    }

    Chunk& chunk = chunks.emplace_back();
    chunk.contents = std::move(norm);
    chunk.kind = Chunk::PlainText;
    chunk.space_before = llvm::isSpace(text.front());
    chunk.space_after = llvm::isSpace(text.back());
    return *this;
}

Paragraph& Paragraph::append_code(llvm::StringRef code, bool preserve) {
    bool adjacent_code = !chunks.empty() && chunks.back().kind == Chunk::InlineCode;
    std::string norm = canonicalize_spaces(code);
    if(norm.empty()) {
        return *this;
    }

    Chunk& chunk = chunks.emplace_back();
    chunk.contents = std::move(norm);
    chunk.kind = Chunk::InlineCode;
    chunk.preserve = preserve;
    // Disallow adjacent code spans without spaces, markdown can't render them.
    chunk.space_before = adjacent_code;
    return *this;
}

BulletList::BulletList() = default;
BulletList::~BulletList() = default;

void BulletList::render_markdown(llvm::raw_ostream& os) const {
    for(auto& item: items) {
        // Instead of doing this we might prefer passing the indent to children
        // to get rid of the copies, if it turns out to be a bottleneck.
        os << "- " << indent_lines(item.as_markdown()) << '\n';
    }
    // We need a new line after list to terminate it in markdown.
    os << '\n';
}

void BulletList::render_plain_text(llvm::raw_ostream& os) const {
    for(auto& item: items) {
        // Instead of doing this we might prefer passing the indent to children
        // to get rid of the copies, if it turns out to be a bottleneck.
        os << "- " << indent_lines(item.as_plain_text()) << '\n';
    }
}

std::unique_ptr<Block> BulletList::clone() const {
    return std::make_unique<BulletList>(*this);
}

Document& BulletList::add_item() {
    return items.emplace_back();
}

Document& Document::operator=(const Document& other) {
    if(this == &other) {
        return *this;
    }

    children.clear();
    for(const auto& child: other.children) {
        children.push_back(child->clone());
    }
    return *this;
}

void Document::append(Document other) {
    std::ranges::move(other.children, std::back_inserter(children));
}

Paragraph& Document::add_paragraph() {
    children.push_back(std::make_unique<Paragraph>());
    return *static_cast<Paragraph*>(children.back().get());
}

void Document::add_ruler() {
    children.push_back(std::make_unique<Ruler>());
}

void Document::add_code_block(std::string code, std::string language) {
    children.emplace_back(std::make_unique<CodeBlock>(std::move(code), std::move(language)));
}

Paragraph& Document::add_heading(std::size_t level) {
    assert(level > 0);
    children.emplace_back(std::make_unique<Heading>(level));
    return *static_cast<Paragraph*>(children.back().get());
}

BulletList& Document::add_bullet_list() {
    children.emplace_back(std::make_unique<BulletList>());
    return *static_cast<BulletList*>(children.back().get());
}

std::string Document::as_markdown() const {
    return render_blocks(children, &Block::render_markdown);
}

std::string Document::as_plain_text() const {
    return render_blocks(children, &Block::render_plain_text);
}

}  // namespace clice::markup
