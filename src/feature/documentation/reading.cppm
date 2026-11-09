/// What the documentation of a declaration and of an indexed symbol share:
/// choosing the comment among those of the entity's declarations, and
/// reading its text, `@copydoc`s followed.

module clice:feature.documentation.reading;

import :feature.documentation;
import :semantic.comment;
import :support.prelude;
import :syntax.lexical_scan;

namespace clice::feature {

/// Of the comments attached to the declarations of one entity, offered in
/// order, the one documenting the entity: the first Doxygen comment, else
/// the first of any style. Once a Doxygen comment is offered the choice
/// stays.
class CommentChoice {
public:
    /// Offer the comments attached to one declaration of `content`
    /// (attached_comments); true once the choice is made.
    bool offer(llvm::StringRef content, llvm::ArrayRef<LexicalInfo::Comment> run) {
        if(doxygen || run.empty()) {
            return doxygen;
        }
        auto first = run.front().range;
        if(is_doxygen_comment(content.slice(first.begin, first.end))) {
            text = comment_text(content, run);
            doxygen = true;
        } else if(text.empty()) {
            text = comment_text(content, run);
        }
        return doxygen;
    }

    bool decided() const {
        return doxygen;
    }

    bool empty() const {
        return text.empty();
    }

    /// The text (comment_text) of the comment chosen, empty without one.
    std::string take() && {
        return std::move(text);
    }

private:
    std::string text;
    bool doxygen = false;
};

/// How many `@copydoc`s deep documentation is followed: a cycle ends there.
constexpr unsigned copy_depth = 4;

inline Documentation read(std::string text,
                          const DocumentationOptions& options,
                          CopiedDocLookup lookup) {
    if(options.format == "plain") {
        return {.details = std::move(text), .verbatim = true};
    }
    return parse_documentation(text, lookup);
}

inline std::optional<Documentation> nothing_copied(llvm::StringRef) {
    return std::nullopt;
}

/// Read `text` as `options` say, each `@copydoc` taking the documentation
/// (`document`, one level deeper) of the first of the targets it names
/// (`targets`) that has one.
Documentation read_copying(std::string text,
                           const DocumentationOptions& options,
                           unsigned depth,
                           auto targets,
                           auto document) {
    auto copied = [&](llvm::StringRef name) -> std::optional<Documentation> {
        if(depth == copy_depth) {
            return std::nullopt;
        }
        for(const auto& target: targets(name)) {
            if(auto doc = document(target, depth + 1); !doc.empty()) {
                return doc;
            }
        }
        return std::nullopt;
    };
    return read(std::move(text), options, copied);
}

}  // namespace clice::feature
