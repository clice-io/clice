[[noreturn]] void fail(const char* reason);

int first_positive(const int* values, int count) {
    for(int i = 0; i < count; i += 1) {
        if(values[i] > 0) {
            return values[i];
        }
    }
    fail("none");
}
