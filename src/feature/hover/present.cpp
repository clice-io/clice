module;

#include "support/prelude.h"

module clice;

import :feature.documentation;
import :feature.hover;
import :semantic.display;
import :semantic.symbol;
import :support.markup;
import :syntax.documentation;

namespace clice::feature {

namespace {

/// Sizes (and padding) are shown in bytes if possible, otherwise in bits.
auto format_size(std::uint64_t bits) -> std::string {
    auto [value, unit] = bits % 8 == 0 ? std::pair(bits / 8, "byte") : std::pair(bits, "bit");
    return llvm::formatv("{0} {1}{2}", value, unit, value == 1 ? "" : "s").str();
}

/// An offset in bytes, and bits past them for a bitfield, so offsets of
/// different fields compare at a glance.
auto format_offset(std::uint64_t bits) -> std::string {
    auto offset = format_size(bits / 8 * 8);
    if(bits % 8 != 0) {
        offset += " and " + format_size(bits % 8);
    }
    return offset;
}

/// Plain reStructuredText with its inline markup taken out: backslash
/// escapes, *emphasis*, **strong** and the trailing underscore of a
/// reference_.
auto rst_plain(llvm::StringRef text) -> std::string {
    constexpr llvm::StringLiteral opening = " \t([{<'\"-/:";
    constexpr llvm::StringLiteral closing = " \t)]}>'\"-/:.,;!?";

    std::string plain;
    for(std::size_t i = 0; i < text.size(); i += 1) {
        char c = text[i];
        if(c == '\\' && i + 1 < text.size()) {
            i += 1;
            if(!llvm::isSpace(text[i])) {
                plain += text[i];
            }
            continue;
        }

        if(c == '*' && (i == 0 || opening.contains(text[i - 1]))) {
            auto marker = text.substr(i).starts_with("**") ? llvm::StringRef("**") : "*";
            auto inner = i + marker.size();
            auto end = text.find(marker, inner);
            if(inner < text.size() && !llvm::isSpace(text[inner]) && end != llvm::StringRef::npos &&
               !llvm::isSpace(text[end - 1]) &&
               (end + marker.size() == text.size() ||
                closing.contains(text[end + marker.size()]))) {
                plain += rst_plain(text.slice(inner, end));
                i = end + marker.size() - 1;
                continue;
            }
        }

        if(c == '_' && i > 0 && llvm::isAlnum(text[i - 1]) &&
           (i + 1 == text.size() || closing.contains(text[i + 1]))) {
            continue;
        }
        plain += c;
    }
    return plain;
}

/// The text of inline reStructuredText: ``literals`` and interpreted text
/// as code, links and cross references (`text <target>`_, :ref:`text
/// <target>`) as their text.
void parse_rst_inline(llvm::StringRef text, markup::Paragraph& out) {
    while(!text.empty()) {
        auto tick = text.find('`');
        if(tick == llvm::StringRef::npos) {
            out.append_text(rst_plain(text));
            return;
        }

        if(text.substr(tick).starts_with("``")) {
            auto end = text.find("``", tick + 2);
            if(end == llvm::StringRef::npos) {
                out.append_text(rst_plain(text));
                return;
            }
            out.append_text(rst_plain(text.substr(0, tick)));
            out.append_code(text.slice(tick + 2, end));
            text = text.substr(end + 2);
            continue;
        }

        auto end = text.find('`', tick + 1);
        if(end == llvm::StringRef::npos) {
            out.append_text(rst_plain(text));
            return;
        }

        /// A role names its kind before the backtick, as in :ref:`...`.
        auto before = text.substr(0, tick);
        bool role = false;
        if(before.ends_with(":")) {
            auto start = before.drop_back().rfind(':');
            if(start != llvm::StringRef::npos &&
               ranges::all_of(before.slice(start + 1, before.size() - 1),
                              [](char c) { return llvm::isAlpha(c) || c == '-'; })) {
                before = before.substr(0, start);
                role = true;
            }
        }
        out.append_text(rst_plain(before));

        auto content = text.slice(tick + 1, end);
        text = text.substr(end + 1);
        bool link = text.starts_with("_");
        while(text.consume_front("_")) {}

        if(!role && !link) {
            /// Interpreted text without a role: the manual writes flags and
            /// names this way.
            out.append_code(content);
            continue;
        }
        if(content.ends_with(">")) {
            if(auto target = content.rfind('<'); target != llvm::StringRef::npos) {
                content = content.substr(0, target).rtrim();
            }
        }
        out.append_text(content);
    }
}

/// Renders clang's attribute documentation, which is reStructuredText
/// written for its manual: paragraphs, list items, code blocks and the
/// note and warning directives.
void parse_rst(llvm::StringRef input, markup::Document& output) {
    llvm::SmallVector<llvm::StringRef> lines;
    input.split(lines, '\n');

    auto indent_of = [](llvm::StringRef line) {
        return line.size() - line.ltrim(' ').size();
    };

    /// The lines after `from` indented deeper than `indent`, blank lines
    /// among them, as a block without its common indentation.
    auto take_block = [&](std::size_t& from, std::size_t indent) {
        std::size_t end = from;
        std::size_t common = std::string::npos;
        for(std::size_t i = from; i < lines.size(); i += 1) {
            if(lines[i].trim().empty()) {
                continue;
            }
            if(indent_of(lines[i]) <= indent) {
                break;
            }
            common = std::min(common, indent_of(lines[i]));
            end = i + 1;
        }

        std::string block;
        for(std::size_t i = from; i < end; i += 1) {
            if(!block.empty() || !lines[i].trim().empty()) {
                block += lines[i].trim().empty() ? "" : lines[i].substr(common).rtrim();
                block += '\n';
            }
        }
        from = end;
        return llvm::StringRef(block).rtrim().str();
    };

    std::string paragraph;
    auto flush = [&] {
        if(auto text = llvm::StringRef(paragraph).trim(); !text.empty()) {
            parse_rst_inline(text, output.add_paragraph());
        }
        paragraph.clear();
    };

    /// The number of the last `#.` item of the list going on, 0 outside one.
    std::size_t numbered = 0;
    for(std::size_t i = 0; i < lines.size();) {
        auto text = lines[i].trim();
        if(text.empty()) {
            flush();
            i += 1;
            continue;
        }

        if(text.consume_front(".. ")) {
            flush();
            auto indent = indent_of(lines[i]);
            auto [directive, argument] = text.split("::");
            argument = argument.trim();
            i += 1;
            auto body = take_block(i, indent);
            if(directive == "code-block" || directive == "code") {
                output.add_code_block(std::move(body), argument == "c++" ? "cpp" : argument.str());
            } else if(directive.equals_insensitive("note") ||
                      directive.equals_insensitive("warning")) {
                paragraph = (directive.equals_insensitive("note") ? "Note: " : "Warning: ") +
                            argument.str() + " " + body;
                flush();
            }
            continue;
        }

        bool auto_numbered = text.starts_with("#. ");
        bool item = auto_numbered || text.starts_with("- ") || text.starts_with("* ") ||
                    (llvm::isDigit(text[0]) && text.drop_while(llvm::isDigit).starts_with(". "));
        if(item || paragraph.empty()) {
            flush();
            if(auto_numbered) {
                numbered += 1;
                paragraph = std::to_string(numbered);
                text = text.drop_front(1);
            } else if(indent_of(lines[i]) == 0) {
                numbered = 0;
            }
        }
        paragraph += text;
        paragraph += ' ';
        i += 1;
    }
    flush();
}

}  // namespace

/// Exhaustive so a new kind fails to compile rather than silently lose its
/// noun.
auto kind_noun(SymbolKind kind) -> llvm::StringRef {
    switch(kind) {
        case SymbolKind::Module: return "module";
        case SymbolKind::Namespace: return "namespace";
        case SymbolKind::Class: return "class";
        case SymbolKind::Struct: return "struct";
        case SymbolKind::Union: return "union";
        case SymbolKind::Enum: return "enum";
        case SymbolKind::Type: return "type";
        case SymbolKind::Concept: return "concept";
        case SymbolKind::Field: return "field";
        case SymbolKind::EnumMember: return "enumerator";
        /// Only an index card has it: whether the operator is a member is
        /// not stored.
        case SymbolKind::Operator:
        case SymbolKind::Function: return "function";
        case SymbolKind::Method: return "method";
        case SymbolKind::Variable: return "variable";
        case SymbolKind::Parameter: return "parameter";
        case SymbolKind::Label: return "label";
        case SymbolKind::Macro: return "macro";
        case SymbolKind::Header: return "header";
        case SymbolKind::Attribute: return "attribute";
        case SymbolKind::Number:
        case SymbolKind::Character:
        case SymbolKind::String: return "literal";
        case SymbolKind::Comment:
        case SymbolKind::Keyword:
        case SymbolKind::Directive:
        case SymbolKind::Invalid: return "";
    }
    std::unreachable();
}

auto to_protocol(const HoverInfo& info,
                 protocol::MarkupKind kind,
                 const PositionMap& map,
                 PositionEncoding encoding) -> protocol::Hover {
    auto document = info.present();

    protocol::Hover result{
        .contents =
            protocol::MarkupContent{
                                    .kind = kind,
                                    .value = kind == protocol::MarkupKind::Markdown ? document.as_markdown()
                                                                : document.as_plain_text(),
                                    },
    };

    if(info.symbol_range) {
        result.range = map.range(*info.symbol_range, encoding);
    }

    return result;
}

markup::Document HoverInfo::present() const {
    markup::Document output;

    /// One line names the entity and where it is: `function translate ·
    /// namespace geo`.
    auto& title = output.add_paragraph();
    if(auto word = kind_noun(kind); !word.empty()) {
        title.append_strong(access.empty() ? word.str() : access + " " + word.str());
        title.append_space().append_code(name);
    } else {
        title.append_strong(name);
    }
    if(!scope_kind.empty()) {
        title.append_text(" · " + scope_kind + " ");
        if(!scope.empty()) {
            title.append_code(scope);
        }
    }
    output.add_spacing();

    add_summary(output, documentation, deprecation);

    if(!definition.empty()) {
        output.add_code_block(definition);
    }
    if(!target_definition.empty()) {
        output.add_code_block(target_definition);
    }
    if(!expansion.empty()) {
        output.add_paragraph().append_text("Expands to:");
        output.add_code_block(expansion);
    }
    if(!file.empty()) {
        output.add_paragraph().append_code(file);
    }

    if(type) {
        auto& line = output.add_paragraph().append_text("Type: ").append_code(type->text);
        if(type->aka) {
            line.append_text(" (aka ").append_code(*type->aka).append_text(")");
        }
    }
    for(const auto& alias: aliases) {
        output.add_paragraph().append_code(alias.text).append_text(" is ").append_code(*alias.aka);
    }
    if(value) {
        output.add_paragraph().append_text("Value: ").append_code(*value);
    }
    if(length) {
        output.add_paragraph().append_text(llvm::formatv("Length: {0}", *length).str());
    }
    if(defined_on_line) {
        output.add_paragraph().append_text(
            llvm::formatv("Not defined yet here: defined on line {0}", *defined_on_line).str());
    }
    if(!overrides.empty()) {
        auto& line = output.add_paragraph().append_text("Overrides: ");
        for(auto [index, overridden]: views::enumerate(overrides)) {
            if(index != 0) {
                line.append_text(", ");
            }
            line.append_code(overridden);
        }
    }
    if(offset) {
        output.add_paragraph().append_text("Offset: " + format_offset(*offset));
    }
    if(size) {
        auto& line = output.add_paragraph().append_text("Size: " + format_size(*size));
        if(padding && *padding != 0) {
            /// A field's padding follows it; a class's is spread inside it.
            line.append_text(
                llvm::formatv(kind == SymbolKind::Field ? " (+{0} padding)" : " ({0} padding)",
                              format_size(*padding))
                    .str());
        }
        if(align) {
            line.append_text(", alignment " + format_size(*align));
        }
    }
    if(callee_arg_info) {
        assert(pass_mode);
        auto& line = output.add_paragraph().append_text("Passed ");
        if(callee_arg_info->name) {
            line.append_text("as ").append_code(*callee_arg_info->name);
        }
        switch(*pass_mode) {
            case PassMode::Value: line.append_text(" by value"); break;
            case PassMode::Ref: line.append_text(" by reference"); break;
            case PassMode::ConstRef: line.append_text(" by const reference"); break;
            case PassMode::RValueRef: line.append_text(" by rvalue reference"); break;
        }
        if(converted_to) {
            line.append_text(", converted to ").append_code(*converted_to);
        }
    }

    if(!documentation.template_params.empty() || !documentation.params.empty() ||
       !documentation.returns.empty() || !documentation.retvals.empty() ||
       !documentation.throws.empty()) {
        output.add_spacing();
    }
    add_parameters(output, "Template parameters: ", documentation.template_params);
    add_parameters(output, "Parameters: ", documentation.params);
    if(!documentation.returns.empty()) {
        output.add_paragraph().append_text("Returns: ").append_markdown(documentation.returns);
    }
    add_entries(output, "Return values: ", documentation.retvals);
    add_entries(output, "Throws: ", documentation.throws);

    if(!attribute_documentation.empty()) {
        output.add_ruler();
        parse_rst(attribute_documentation, output);
    }
    if(!documentation.details.empty() || !documentation.see.empty()) {
        output.add_ruler();
        add_details(output, documentation);
    }
    if(!attribute_macros.empty()) {
        output.add_spacing();
    }
    for(const auto& macro: attribute_macros) {
        output.add_paragraph().append_code(macro.use).append_text(" is ").append_code(
            macro.attributes);
    }
    return output;
}

}  // namespace clice::feature
