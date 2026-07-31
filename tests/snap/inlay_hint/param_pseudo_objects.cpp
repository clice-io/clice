/// # Parameter Hints
///
/// ## Pseudo-object expressions — MS property accesses stay quiet; written subscripts keep the accessor's names
///
/// - status: supported
/// - order: 13

int printf(const char* Format, ...);

struct State {
    __declspec(property(get = GetX, put = PutX)) int x[];
    int GetX(int row, int column);
    void PutX(int value);

    // The syntactic form is a binary operator: no `value:` hint on `y`.
    void Work(int y) {
        x = y;
    }
};

int use() {
    State s;
    // The semantic form of __builtin_dump_struct calls printf; none of it
    // is written here.
    __builtin_dump_struct(&s, printf);
    printf("%d", 42);
    // Property subscripts read best with the accessor's parameter names.
    return s.x[1][2];
}
