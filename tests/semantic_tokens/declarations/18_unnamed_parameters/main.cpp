// An unnamed parameter gives no name token.
void take_one(int§(close_paren)) {}
void take_two(int§(comma), char* c) {}

template <class§(template_close)>
struct Tag {};
