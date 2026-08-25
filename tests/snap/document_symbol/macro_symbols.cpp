/// # Location Correctness
///
/// ## Symbols from macro expansions — a symbol produced by a macro invocation is located at the invocation, not at the macro definition
///
/// - status: supported
/// - issues: clangd#475
/// - order: 1

// The assertion holds the directives out of the preamble region, whose
// live record the server path does not yet see.
static_assert(true);

#define DEFINE_HANDLER(name) void name()

DEFINE_HANDLER(on_ready);
DEFINE_HANDLER(on_close);

#define DECLARE_CLASS(X) class X
DECLARE_CLASS(Generated) {
    int member;
};
