// Names that clash where only the compiler sees it: a parameter and a local
// of one function, two parameters, two functions of one signature, two labels
// of one function.
int scale(int §(parameter)factor, int §(second)offset) {
    int §(local)result = factor * 2 + offset;
    return result;
}

int §(compute)compute(int v) { return v; }
int twin(int v) { return v + 1; }

void spin() {
§(again)again:
done:
    goto again;
}

int main() { return scale(1, 2) + compute(3) + twin(4); }
