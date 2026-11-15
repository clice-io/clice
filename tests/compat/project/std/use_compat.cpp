import std.compat;

int use_compat() {
    ::size_t length = ::strlen("compat");
    return static_cast<int>(length) + ::abs(-1);
}
