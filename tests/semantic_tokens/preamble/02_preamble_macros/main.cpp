#define §(empty_definition)TRACED
#define §(definition)SQUARE(x) ((x) * (x))

[[maybe_unused]] static int squared = §(expansion)SQUARE(4);
