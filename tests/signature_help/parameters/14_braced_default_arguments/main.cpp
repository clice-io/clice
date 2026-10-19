struct Options {
    Options();
    Options(int level, bool verbose);
};

struct Range {
    int begin;
    int end;
};

void configure(Options options = {}, Options tuned = {2, true}, Range range = {0, 10}, int count = {});

int main() {
    configure(§(braces));
}
