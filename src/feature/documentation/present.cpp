module clice;

import :feature.documentation;
import :support.markup;

namespace clice::feature {

namespace {

/// Write after an entry — a parameter, a return value, an exception — what
/// a documentation says of it: its direction, then its text.
void describe(markup::Paragraph& entry, llvm::StringRef direction, llvm::StringRef text) {
    if(!direction.empty()) {
        entry.append_text(" (" + direction.str() + ")");
    }
    if(!text.empty()) {
        entry.append_text(" — ").append_markdown(text);
    }
}

void add_parameters(markup::Document& output,
                    llvm::StringRef title,
                    llvm::ArrayRef<Documentation::Param> documented) {
    if(documented.empty()) {
        return;
    }
    output.add_paragraph().append_strong(title);
    auto& list = output.add_bullet_list();
    for(const auto& param: documented) {
        describe(list.add_item().add_paragraph().append_code(param.name),
                 param.direction,
                 param.text);
    }
}

void add_entries(markup::Document& output, llvm::ArrayRef<Documentation::Entry> entries) {
    auto& items = output.add_bullet_list();
    for(const auto& entry: entries) {
        describe(items.add_item().add_paragraph().append_code(entry.name), {}, entry.text);
    }
}

}  // namespace

void add_sections(markup::Document& output, const Documentation& documentation) {
    add_parameters(output, "Template parameters:", documentation.template_params);
    add_parameters(output, "Parameters:", documentation.params);
    if(!documentation.returns.empty() || !documentation.retvals.empty()) {
        output.add_paragraph()
            .append_strong("Returns:")
            .append_space()
            .append_markdown(documentation.returns);
        if(!documentation.retvals.empty()) {
            add_entries(output, documentation.retvals);
        }
    }
    if(!documentation.throws.empty()) {
        output.add_paragraph().append_strong("Throws:");
        add_entries(output, documentation.throws);
    }
}

void add_details(markup::Document& output, const Documentation& documentation) {
    if(!documentation.details.empty()) {
        if(documentation.verbatim) {
            output.add_lines(documentation.details);
        } else {
            output.add_markdown(documentation.details);
        }
    }
    if(!documentation.see.empty()) {
        output.add_paragraph()
            .append_strong("See also:")
            .append_space()
            .append_markdown(llvm::join(documentation.see, ", "));
    }
}

void add_summary(markup::Document& output,
                 const Documentation& documentation,
                 std::optional<llvm::StringRef> attribute) {
    if(attribute || documentation.deprecated) {
        auto& paragraph = output.add_paragraph().append_text("Deprecated");
        if(attribute && !attribute->empty()) {
            paragraph.append_text(": " + attribute->str());
        } else if(documentation.deprecated && !documentation.deprecated->empty()) {
            paragraph.append_text(": ").append_markdown(*documentation.deprecated);
        }
    }
    if(!documentation.brief.empty()) {
        output.add_markdown(documentation.brief);
    }
}

auto present(const Documentation& documentation) -> markup::Document {
    markup::Document output;
    add_summary(output, documentation, std::nullopt);
    add_sections(output, documentation);
    add_details(output, documentation);
    return output;
}

}  // namespace clice::feature
