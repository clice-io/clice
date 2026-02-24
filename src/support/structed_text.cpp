#include "support/structed_text.h"

#include <algorithm>
#include <cctype>
#include <iterator>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

std::string Block::as_markdown() const {
    std::string md;
    llvm::raw_string_ostream os(md);
    render_markdown(os);
    return llvm::StringRef(os.str()).trim().str();
}

BulletList::BulletList() = default;
BulletList::~BulletList() = default;

std::unique_ptr<Block> BulletList::clone() const {
    return std::make_unique<BulletList>(*this);
}

void BulletList::render_markdown(llvm::raw_ostream& os) const {
    for(auto& item: items) {
        os << "- " << item.as_markdown() << '\n';
    }
}

StructedText& BulletList::add_item() {
    return items.emplace_back();
}

// Clangd inserts escape char '\' before '*', '-' and other markdown markers
// That causes markdown comments are escaped and cannot be rendered properly
// on editors
// We do nothing on it. All the left comments are regarded as markdown rather
// than plain text
void Paragraph::render_markdown(llvm::raw_ostream& os) const {
    bool need_space = false;
    bool has_chunks = false;
    for(auto& chunk: chunks) {
        if(chunk.space_ahead || need_space) {
            os << ' ';
        }
        switch(chunk.kind) {
            case Kind::Bold: {
                os << "**" << chunk.content << "**";
                break;
            }
            case Kind::Italic: {
                os << '*' << chunk.content << '*';
                break;
            }
            case Kind::InlineCode: {
                os << '`' << chunk.content << '`';
                break;
            }
            case Kind::Strikethough: {
                os << "~~" << chunk.content << "~~";
                break;
            }
            default: {
                // Kind::PlainText
                os << chunk.content;
                break;
            }
        }
        has_chunks = true;
        need_space = chunk.space_after;
    }
}

Paragraph& Paragraph::append_text(std::string text, Kind kind) {
    if(kind == Kind::PlainText) {
        llvm::StringRef s{text};
        // s = s.trim(" \t\v\f\r");
        if(s.empty()) {
            return *this;
        }
        bool flag = !chunks.empty() && !std::isspace(chunks.back().content.back());
        auto& chunk = chunks.emplace_back();
        chunk.kind = Kind::PlainText;
        chunk.content = std::move(s.str());
        chunk.space_ahead = flag;
        chunk.space_after = !std::isspace(s.back());
    } else {
        bool flag = !chunks.empty() && chunks.back().kind != Kind::PlainText;
        auto& chunk = chunks.emplace_back();
        chunk.kind = kind;
        chunk.content = std::move(text);
        chunk.space_ahead = flag;
    }
    return *this;
}

Paragraph& Paragraph::append_newline_char(unsigned cnt) {
    auto& chunk = chunks.emplace_back();
    chunk.kind = Kind::PlainText;
    chunk.content = std::string(cnt, '\n');
    return *this;
}

class Heading : public Paragraph {
public:
    Heading(unsigned level) : level(level) {}

    void render_markdown(llvm::raw_ostream& os) const override {
        os << std::string(level, '#') << ' ';
        Paragraph::render_markdown(os);
    }

private:
    unsigned level;
};

class Ruler : public Block {
public:
    void render_markdown(llvm::raw_ostream& os) const override {
        os << "\n---\n";
    }

    bool is_ruler() const override {
        return true;
    }

    std::unique_ptr<Block> clone() const override {
        return std::make_unique<Ruler>(*this);
    }
};

class CodeBlock : public Block {
public:
    void render_markdown(llvm::raw_ostream& os) const override {
        os << "```" << lang << '\n' << code << "```\n";
    }

    std::unique_ptr<Block> clone() const override {
        return std::make_unique<CodeBlock>(*this);
    }

    CodeBlock(std::string code, std::string lang = "") :
        code(std::move(code)), lang(std::move(lang)) {};

private:
    std::string lang;
    std::string code;
};

static std::string render_blocks(llvm::ArrayRef<std::unique_ptr<Block>> blocks) {
    std::string md;
    llvm::raw_string_ostream os(md);

    // Trim rulers.
    blocks = blocks.drop_while([](const std::unique_ptr<Block>& C) { return C->is_ruler(); });
    auto last = llvm::find_if(llvm::reverse(blocks),
                              [](const std::unique_ptr<Block>& C) { return !C->is_ruler(); });
    blocks = blocks.drop_back(blocks.end() - last.base());

    bool last_block_was_ruler = true;
    // render
    for(const auto& b: blocks) {
        if(b->is_ruler() && last_block_was_ruler) {
            continue;
        }
        last_block_was_ruler = b->is_ruler();
        b->render_markdown(os);
    }

    // Get rid of redundant empty lines introduced in plaintext while imitating
    // padding in markdown.
    std::string adjusted_result;
    llvm::StringRef trimmed_text(os.str());
    trimmed_text = trimmed_text.trim(" \t\v\f\r");

    llvm::copy_if(trimmed_text,
                  std::back_inserter(adjusted_result),
                  [&trimmed_text](const char& C) {
                      return !llvm::StringRef(trimmed_text.data(), &C - trimmed_text.data() + 1)
                                  // We allow at most two newlines.
                                  .ends_with("\n\n\n");
                  });

    return adjusted_result;
}

void StructedText::append(StructedText& other) {
    std::move(other.blocks.begin(), other.blocks.end(), std::back_inserter(blocks));
}

Paragraph& StructedText::add_paragraph() {
    blocks.emplace_back(std::make_unique<Paragraph>());
    return *static_cast<Paragraph*>(blocks.back().get());
}

void StructedText::add_ruler() {
    blocks.push_back(std::make_unique<Ruler>());
}

void StructedText::add_code_block(std::string code, std::string lang) {
    blocks.emplace_back(std::make_unique<CodeBlock>(std::move(code), std::move(lang)));
}

Paragraph& StructedText::add_heading(unsigned level) {
    blocks.emplace_back(std::make_unique<Heading>(level));
    return *static_cast<Paragraph*>(blocks.back().get());
}

BulletList& StructedText::add_bullet_list() {
    blocks.push_back(std::make_unique<BulletList>());
    return *static_cast<BulletList*>(blocks.back().get());
}

std::string StructedText::as_markdown() const {
    return render_blocks(blocks);
}

}  // namespace clice
