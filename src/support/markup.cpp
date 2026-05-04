#include "support/markup.h"

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
        auto content = item.as_markdown();
        os << "- ";
        for(size_t i = 0; i < content.size(); ++i) {
            os << content[i];
            if(content[i] == '\n' && i + 1 < content.size())
                os << "  ";
        }
        os << '\n';
    }
}

Markup& BulletList::add_item() {
    return items.emplace_back();
}

void Paragraph::render_markdown(llvm::raw_ostream& os) const {
    bool need_space = false;
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
            case Kind::Strikethrough: {
                os << "~~" << chunk.content << "~~";
                break;
            }
            default: {
                os << chunk.content;
                break;
            }
        }
        need_space = chunk.space_after;
    }
}

Paragraph& Paragraph::append_text(std::string text, Kind kind) {
    if(kind == Kind::PlainText) {
        llvm::StringRef s{text};
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

    std::unique_ptr<Block> clone() const override {
        return std::make_unique<Heading>(*this);
    }

private:
    unsigned level;
};

class Ruler : public Block {
public:
    void render_markdown(llvm::raw_ostream& os) const override {
        os << "---\n";
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
        os << "```" << lang << '\n' << code;
        if(!code.empty() && code.back() != '\n')
            os << '\n';
        os << "```\n";
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
    for(const auto& b: blocks) {
        if(b->is_ruler() && last_block_was_ruler) {
            continue;
        }
        last_block_was_ruler = b->is_ruler();
        b->render_markdown(os);
        os << '\n';
    }

    // Collapse runs of 3+ newlines down to 2 (one blank line max).
    std::string result;
    llvm::StringRef text(os.str());
    text = text.trim();

    llvm::copy_if(text, std::back_inserter(result), [&text](const char& C) {
        return !llvm::StringRef(text.data(), &C - text.data() + 1).ends_with("\n\n\n");
    });

    return result;
}

void Markup::append(Markup& other) {
    std::move(other.blocks.begin(), other.blocks.end(), std::back_inserter(blocks));
}

Paragraph& Markup::add_paragraph() {
    blocks.emplace_back(std::make_unique<Paragraph>());
    return *static_cast<Paragraph*>(blocks.back().get());
}

void Markup::add_ruler() {
    blocks.push_back(std::make_unique<Ruler>());
}

void Markup::add_code_block(std::string code, std::string lang) {
    blocks.emplace_back(std::make_unique<CodeBlock>(std::move(code), std::move(lang)));
}

Paragraph& Markup::add_heading(unsigned level) {
    blocks.emplace_back(std::make_unique<Heading>(level));
    return *static_cast<Paragraph*>(blocks.back().get());
}

BulletList& Markup::add_bullet_list() {
    blocks.push_back(std::make_unique<BulletList>());
    return *static_cast<BulletList*>(blocks.back().get());
}

std::string Markup::as_markdown() const {
    return render_blocks(blocks);
}

}  // namespace clice
