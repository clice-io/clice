// clang underlines both operands of an invalid + and puts the caret on the
// operator: no range holds it, so the caret's token stands. A discarded
// result is underlined whole, the caret at its start.
double f() {
    return 1.0 + "a";
}
[[nodiscard]] int value();
void g() {
    value();
}
