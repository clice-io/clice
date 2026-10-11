int §(variable)shared_value = 0;

int reader() {
    return shared_value;
}

int writer() {
    shared_value = 1;
    return shared_value;
}
