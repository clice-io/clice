#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "llvm/Support/raw_ostream.h"

namespace clice {

/// Base class of markup blocks
class Block {
public:
    virtual void render_markdown(llvm::raw_ostream& os) const = 0;
    virtual std::unique_ptr<Block> clone() const = 0;
    std::string as_markdown() const;

    virtual bool is_ruler() const {
        return false;
    }

    virtual ~Block() = default;
};

/// Normal text and inline code
class Paragraph : public Block {
public:
    enum class Kind : uint8_t {
        Bold,
        Italic,
        PlainText,
        InlineCode,
        Strikethrough,
    };
    void render_markdown(llvm::raw_ostream& os) const override;

    std::unique_ptr<Block> clone() const override {
        return std::make_unique<Paragraph>(*this);
    }

    Paragraph& append_text(std::string text, Kind kind = Kind::PlainText);

    Paragraph& append_newline_char(unsigned cnt = 1);

private:
    struct Chunk {
        Kind kind;
        std::string content;
        bool space_ahead = false;
        bool space_after = false;
    };

    std::vector<Chunk> chunks;
};

class Markup;

/// Allow nested structure
class BulletList : public Block {
public:
    BulletList();
    ~BulletList();
    void render_markdown(llvm::raw_ostream& os) const override;

    std::unique_ptr<Block> clone() const override;

    Markup& add_item();

private:
    std::vector<Markup> items;
};

class Markup {
public:
    Markup() = default;

    Markup(const Markup& other) {
        *this = other;
    }

    Markup(Markup&&) = default;

    Markup& operator=(const Markup& other) {
        blocks.clear();
        for(auto& b: other.blocks) {
            blocks.push_back(b->clone());
        }
        return *this;
    }

    Markup& operator=(Markup&&) = default;

    void append(Markup& doc);

    Paragraph& add_paragraph();

    void add_ruler();

    void add_code_block(std::string code, std::string lang = "");

    Paragraph& add_heading(unsigned level);

    BulletList& add_bullet_list();

    std::string as_markdown() const;

private:
    std::vector<std::unique_ptr<Block>> blocks;
};

}  // namespace clice
