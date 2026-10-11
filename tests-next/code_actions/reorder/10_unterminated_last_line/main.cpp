// The last line ends the file without a newline: the definition moved
// off it gets one, and its comment never swallows the next definition.

struct §(cls)S {
    void a();
    void b();
};

void S::b() {}
void S::a() {}  // ends the file without a newline