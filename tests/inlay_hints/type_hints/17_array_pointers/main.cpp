// A pointer to an array prints with the parentheses its declarator needs.
int grid[3][4];

void use() {
    auto* row = &grid;
    auto copy = &grid;
}
