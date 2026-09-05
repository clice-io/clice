/// # Macro-shadowed symbol
///
/// - status: supported
///
/// A function-like macro shadows a same-named function at the call site
///
/// The card shows the active macro and its expansion instead of the shadowed
/// function.

namespace shadow {

int lookup(int key) {
    return key;
}

}

#define lookup(key) ((key) + 100)

int value = loo§(shadowed_use)kup(5);
