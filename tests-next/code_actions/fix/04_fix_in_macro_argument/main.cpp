// A warning inside a macro's expansion whose fix would edit both the
// macro's argument and its definition: only the edit in the argument lands,
// and the code no longer compiles.

#define ASSIGN_IF(x) if (x = 1) {}

int check(int value) {
    §(in_macro)ASSIGN_IF(value);
    return value;
}
