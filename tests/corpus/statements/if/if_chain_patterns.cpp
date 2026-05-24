// if-else chain patterns and compound conditions
namespace if_chain {

int classify_char(char c) {
    if(c >= 'a' && c <= 'z')
        return 1;  // lowercase
    else if(c >= 'A' && c <= 'Z')
        return 2;  // uppercase
    else if(c >= '0' && c <= '9')
        return 3;  // digit
    else
        return 0;  // other
}

// early return pattern
bool validate(int x, int y, int z) {
    if(x < 0)
        return false;
    if(y < 0)
        return false;
    if(z < 0)
        return false;
    if(x + y + z > 100)
        return false;
    return true;
}

// if with compound statement and multiple effects
int process(int* data, int size) {
    int sum = 0;
    for(int i = 0; i < size; ++i) {
        if(data[i] > 0) {
            sum += data[i];
            data[i] = 0;
        } else if(data[i] < -10) {
            sum -= data[i];
        }
    }
    return sum;
}

void test() {
    [[maybe_unused]] int r1 = classify_char('a');
    [[maybe_unused]] bool r2 = validate(1, 2, 3);
    int arr[] = {1, -20, 3, -5};
    [[maybe_unused]] int r3 = process(arr, 4);
}

}  // namespace if_chain
