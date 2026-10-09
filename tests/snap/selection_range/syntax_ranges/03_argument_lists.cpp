/// # Argument and parameter lists
///
/// - status: supported
///
/// The arguments of a call and the parameters of a function are selected
/// without their parentheses before with them

int combine(int left, int §(param)right);

int total() {
    return combine(1, §(arg)2) + combine(3, 4);
}
