struct Point {
    int x;
    int y;
    int z;
};

Point p{1, 2 + 2};

int coordinates[2] = {7, 8};

template <typename T, int N>
struct Array {
    T __elements[N];
};

Array<int, 2> pair = {0, 1};

Point corners[2] = {1, 2, 3, 4, 5, 6};
