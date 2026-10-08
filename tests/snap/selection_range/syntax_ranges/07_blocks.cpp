/// # Blocks and their contents
///
/// - status: supported
///
/// The statements of a block, the members of a class and the declarations of
/// a namespace are selected without their braces before with them
///
/// A cursor on a blank line inside a block starts from the block's
/// contents.

namespace shapes {

struct Square {
    int §(member)side;
    int area() const;
};

int perimeter(const Square& square) {
    int sides = 4;
    §(blank)
    return sides * square.side;
}

}  // namespace shapes
