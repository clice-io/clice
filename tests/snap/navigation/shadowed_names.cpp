/// - verify: server

int §(global_def)value = 1;

int read_shadowed(int §(param_def)value) {
    int sum = §(param_use)value;
    {
        int §(local_def)value = 3;
        sum += §(local_use)value;
    }
    return sum + ::§(global_use)value;
}
