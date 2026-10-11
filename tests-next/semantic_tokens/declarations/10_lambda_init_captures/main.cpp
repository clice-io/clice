// Lambda init-captures receive variable tokens.
int compute();

auto fn = [§(val_def)val = compute()] {
    return §(val_use)val;
};
