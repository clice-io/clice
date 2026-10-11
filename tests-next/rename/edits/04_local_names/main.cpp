// A parameter or a local renames within its function alone.
int scale(int §(parameter)factor) {
    int §(local)result = factor * 2;
    return result;
}

int other(int factor) {
    int result = factor;
    return result;
}

int main() { return scale(1) + other(2); }
