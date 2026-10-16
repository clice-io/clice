#define TRACE(message)
#define UNUSED
#define RETURN_IF(condition) \
    if(condition)            \
    return

int compute();

int checked(int value) {
    TRACE(compute());
    UNUSED int spare = 0;
    RETURN_IF(value < 0) 0;
    return value;
}
