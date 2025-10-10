#include <iostream>

// Tests bugprone-integer-division
int main() {
    float floatFunc(float);
    int intFunc(int);
    double d;
    int i = 42;

    // Warn, floating-point values expected.
    d = 32 * 8 / (2 + i);
    d = 8 * floatFunc(1 + 7 / 2);
    d = i / (1 << 4);

    // OK, no integer division.
    d = 32 * 8.0 / (2 + i);
    d = 8 * floatFunc(1 + 7.0 / 2);
    d = (double)i / (1 << 4);

    // OK, there are signs of deliberateness.
    d = 1 << (i / 2);
    d = 9 + intFunc(6 * i / 32);
    d = (int)(i / 32) - 8;
    return 0;
}
