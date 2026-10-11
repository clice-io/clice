int accumulate(int base) {
    int total = base;
    total = §(local_use)total + §(param_use)base;
    return total;
}
