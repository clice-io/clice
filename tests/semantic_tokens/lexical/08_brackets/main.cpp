template §(template_open)<typename T>
struct Grid {
    T cells§(array_open)[4];
};

Grid§(argument_open)<int> grid§(brace_open){{1, 2, 3, 4}};

int first§(paren_open)(Grid<int>& grid) {
    return grid.cells§(subscript_open)[0];
}
