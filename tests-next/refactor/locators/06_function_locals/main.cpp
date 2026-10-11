double scaled(double length, int factor) {
    double result = length * factor;
    return result;
}

int main() { return static_cast<int>(scaled(1.0, 2)); }
