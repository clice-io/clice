/// # Go to Definition
///
/// ## Navigate through macro wrappers to the underlying declaration
///
/// - status: supported
/// - verify: server
/// - order: 6
///
/// A name spelled in a macro argument anchors at its spelling, so
/// definition and declaration alternate there exactly as at a plain
/// site, and a later use resolves through the wrapper to the function it
/// declares.

#define DECLARE_HOOK(name) int name(int value)

DECLARE_HOOK(§(decl)notify);

DECLARE_HOOK(§(def)notify) {
    return value + 1;
}

int trigger(int value) {
    return §(use)notify(value);
}
