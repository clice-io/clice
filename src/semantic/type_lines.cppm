module clice:semantic.type_lines;

import :support.prelude;

namespace clice::display {

/// `type` written for a card, as Options::width asks: in lines of at most
/// `width` columns that read left to right. `*`, `&` and `[N]` follow what
/// they apply to (`int[16]&`), a function reads `(params) -> R` with its
/// qualifiers before it, a member pointer `C::-> T`; either is parenthesized
/// when something follows it (`((int) -> void)*`). A part that would nest
/// one function or member pointer in another, or does not fit, is written
/// as a Greek letter and spelled on a line of its own under the line using
/// it, two columns further in: `α = ...`. Parts that fit stay C++ as `policy`
/// prints them.
auto type_lines(clang::ASTContext& context,
                clang::QualType type,
                const clang::PrintingPolicy& policy,
                std::uint32_t width) -> std::string;

}  // namespace clice::display
