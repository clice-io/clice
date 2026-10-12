export module Block;

export {
    int alpha() {
        return 1;
    }

    namespace ns {

    int gamma() {
        return 3;
    }

    }  // namespace ns
}

export namespace math {

int add(int a, int b) {
    return a + b;
}

}  // namespace math
