/// # Keyword documentation
///
/// - status: unsupported
/// - issues: clangd#1862
///
/// Language keywords do not have documentation hover yet
///
/// Hovering a keyword such as `const` or `virtual` produces no card.

namespace keywords {

co§(const_kw)nst int limit = 42;

struct Widget {
    vir§(virtual_kw)tual void draw();
};

}
