/// # Macro-shadowed symbol
///
/// - status: supported
///
/// A function-like macro over a same-named function
///
/// clangd tracks this as clangd#2490; at the call site the function-like
/// macro is active, and clice's card shows that macro and its expansion.

namespace shadow {

int lookup(int key) {
    return key;
}

}

#define lookup(key) ((key) + 100)

int value = loo§(shadowed_use)kup(5);
