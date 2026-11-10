module clice;

import :feature.documentation;
import :feature.hover.card;
import :feature.hover;
import :semantic.comment;
import :semantic.display;
import :semantic.symbol;
import :support.markup;
import :support.text;

namespace clice::feature {

namespace {

/// Sizes (and padding) are shown in bytes if possible, otherwise in bits.
auto format_size(std::uint64_t bits) -> std::string {
    return bits % 8 == 0 ? counted(bits / 8, "byte", "bytes") : counted(bits, "bit", "bits");
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

/// A line of the card stating one fact, its label in bold.
auto add_fact(markup::Document& output, llvm::StringRef label) -> markup::Paragraph& {
    return output.add_paragraph().append_strong(label).append_space();
}

/// Write `type` at the end of `line`, or as code below it when it is laid
/// out over several lines.
void add_type(markup::Document& output, markup::Paragraph& line, llvm::StringRef type) {
    if(type.contains('\n')) {
        output.add_code_block(type.str());
    } else {
        line.append_code(type);
    }
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
        output.add_paragraph().append_strong("Expands to:");
        output.add_code_block(expansion);
    }
    if(!file.empty()) {
        output.add_paragraph().append_code(file);
    }

    if(!template_arguments.empty()) {
        output.add_paragraph().append_strong("Template arguments:");
        std::string block;
        for(const auto& [parameter, argument]: template_arguments) {
            auto lead = parameter + " = ";
            llvm::SmallVector<llvm::StringRef> lines;
            llvm::StringRef(argument).split(lines, '\n');
            for(const auto& [index, line]: views::enumerate(lines)) {
                block += (index == 0 ? lead : std::string(lead.size(), ' ')) + line.str() + "\n";
            }
        }
        block.pop_back();
        output.add_code_block(std::move(block));
    }
    if(type) {
        auto& line = add_fact(output, "Type:");
        bool short_aka = type->aka && !llvm::StringRef(type->text).contains('\n') &&
                         !llvm::StringRef(*type->aka).contains('\n') &&
                         type->text.size() + type->aka->size() + 7 <= card_width;
        add_type(output, line, type->text);
        if(short_aka) {
            line.append_text(" (aka ").append_code(*type->aka).append_text(")");
        } else if(type->aka) {
            add_type(output, add_fact(output, "Aka:"), *type->aka);
        }
    }
    for(const auto& alias: aliases) {
        if(llvm::StringRef(alias.text).contains('\n')) {
            output.add_code_block(alias.text);
            add_type(output, output.add_paragraph().append_text("is "), *alias.aka);
        } else {
            add_type(output,
                     output.add_paragraph().append_code(alias.text).append_text(" is "),
                     *alias.aka);
        }
    }
    if(value) {
        add_fact(output, "Value:").append_code(*value);
    }
    if(length) {
        add_fact(output, "Length:").append_text(std::to_string(*length));
    }
    if(defined_on_line) {
        output.add_paragraph().append_text(
            std::format("Not defined yet here: defined on line {}", *defined_on_line));
    }
    if(!overrides.empty()) {
        auto& line = add_fact(output, "Overrides:");
        for(auto [index, overridden]: views::enumerate(overrides)) {
            if(index != 0) {
                line.append_text(", ");
            }
            line.append_code(overridden);
        }
    }
    if(offset) {
        add_fact(output, "Offset:").append_text(format_offset(*offset));
    }
    if(size) {
        auto& line = add_fact(output, "Size:").append_text(format_size(*size));
        if(padding && *padding != 0) {
            /// A field's padding follows it; a class's is spread inside it.
            line.append_text(std::format(" ({}{} padding)",
                                         kind == SymbolKind::Field ? "+" : "",
                                         format_size(*padding)));
        }
    }
    if(align) {
        add_fact(output, "Alignment:").append_text(format_size(*align));
    }
    if(callee_arg_info) {
        assert(pass_mode);
        auto& line = add_fact(output, "Passed:");
        switch(*pass_mode) {
            case PassMode::Value: line.append_text("by value"); break;
            case PassMode::Ref: line.append_text("by reference"); break;
            case PassMode::ConstRef: line.append_text("by const reference"); break;
            case PassMode::RValueRef: line.append_text("by rvalue reference"); break;
        }
        if(callee_arg_info->name) {
            line.append_text(" to ").append_code(*callee_arg_info->name);
        }
        if(converted_to) {
            add_type(output, line.append_text(", converted to "), *converted_to);
        }
    }

    if(!documentation.template_params.empty() || !documentation.params.empty() ||
       !documentation.returns.empty() || !documentation.retvals.empty() ||
       !documentation.throws.empty()) {
        output.add_spacing();
    }
    add_sections(output, documentation);

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
