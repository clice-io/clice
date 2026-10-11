// Saved with Windows line endings by the test: folds end where they do with
// Unix ones.
/* a block
   comment */
struct Point {
    int x;
};

int read(Point p) {
    return p.x;
}

#if 0
int dead;
#endif

const char* raw = R"(line1
line2)";
