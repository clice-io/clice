// The imported name comes from a macro: the name at the import is the
// macro's expansion.
#define MATH_MODULE Math
import §(macro_name)MATH_MODULE;

int main() {
    return add(1, 2);
}
