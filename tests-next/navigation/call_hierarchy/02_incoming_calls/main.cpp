int §(target)helper(int v) {
    return v;
}

int alpha() {
    return helper(1);
}

int beta() {
    return helper(2) + helper(3);
}
