// A template whose template keyword comes from a macro loses its documentation.

int anchor = 0;

#define TEMPLATE template

/// A documented template function.
TEMPLATE <typename T> void §(macro_template)run(T value);
