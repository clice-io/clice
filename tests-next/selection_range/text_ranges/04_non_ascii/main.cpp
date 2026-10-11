// Columns after and inside non-ASCII text count UTF-16 code units: an
// accented letter is one, an emoji two.
int measure(const char* text, int limit);

int run(int limit) {
    return measure("héllo wörld 😀", §(arg)limit);
}

const char* label = "naïve 😀 §(text)café";
