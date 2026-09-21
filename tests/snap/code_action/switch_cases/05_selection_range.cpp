/// # Selection covering the switch
///
/// - status: supported
///
/// A selection spanning the whole statement offers the same action as a cursor inside it

enum class Kind { A, B, C };

int f(Kind kind) {
    §(whole)⟦switch (kind) {
    case Kind::A:
        return 1;
    }⟧
    return 0;
}
