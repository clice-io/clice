struct §(base)Base {};

template <typename T>
struct CRTP : Base {};

struct Widget : CRTP<Widget> {};
