// A conditional branch that cuts across an access section gives way to the
// section, so every fold nests in the ones around it.
class Tree {
public:
    void insert(int key);
#if __cplusplus >= 201103L
    void emplace(int key);
    void erase(int key);
private:
    int moved;
#endif
    int size;
};

class View {
#ifdef __clang__
public:
#endif
    void begin();
    void end();
    void size();
};
