#define DECLARE(name) int name = 0
#define TWICE(value) ((value) * 2)

DECLARE(§(declared)counter);

int doubled() {
    return TWICE(§(expr)counter + 1)§(closing);
}
