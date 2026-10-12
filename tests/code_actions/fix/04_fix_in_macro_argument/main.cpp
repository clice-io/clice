// A warning inside a macro's expansion whose fix would edit both the
// macro's argument and its definition offers no fix: the edit in the
// argument alone would not compile.

#define ASSIGN_IF(x) if (x = 1) {}

int check(int value) {
    §(in_macro)ASSIGN_IF(value);
    return value;
}
