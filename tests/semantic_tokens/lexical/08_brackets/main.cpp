template §(template_open)<typename T>
struct Grid {
    T cells§(array_open)[4];
};

Grid§(argument_open)<int> grid§(brace_open){{1, 2, 3, 4}};
Grid<Grid<int§(inner_close)>§(outer_close)> nested;

int first§(paren_open)(Grid<int>& grid) {
    return grid.cells§(subscript_open)[0];
}

auto identity = []§(lambda_open)<class T>(T value) { return value; };
