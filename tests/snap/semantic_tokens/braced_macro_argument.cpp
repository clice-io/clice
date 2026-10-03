// - diagnostics: expected

// A braced initializer splits a one-parameter macro's argument at its
// comma. Clang recovers by parenthesizing the argument, and its tokens keep
// their kinds.

struct Pt {
    int x;
    int y;
};

#define CALL(e) e

int main() {
    CALL(Pt{1, 2});
}
