// A function-like macro named like a function shadows it at the call: the card
// is the macro's, with its expansion.

namespace shadow {

int lookup(int key) {
    return key;
}

}

#define lookup(key) ((key) + 100)

int value = loo§(shadowed_use)kup(5);
