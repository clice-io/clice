/// # Special Hover Targets
///
/// ## Keyword documentation — hovering a language keyword shows its description
///
/// - status: unsupported
/// - order: 3
/// - issues: clangd#1862
///
/// Hovering a keyword such as `const` or `virtual` produces no card.

namespace keywords {

co§(const_kw)nst int limit = 42;

struct Widget {
    vir§(virtual_kw)tual void draw();
};

}
