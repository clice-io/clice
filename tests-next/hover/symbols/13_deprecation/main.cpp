[[deprecated("use open_v2")]] void §(declaration)open_v1();

void open_v2();

void reopen() {
    §(call)open_v1();
}
