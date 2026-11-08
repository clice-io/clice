struct Command {
    int opcode;
    union {
        struct {
            int lba;
        } read;
        struct {
            int nlb;
        } compare;
    };
};

static union {
    int as_int;
    float as_float;
};

int convert() {
    as_float = 1;
    return §(global)as_int + int(as_float);
}

int submit(Command& command) {
    command.§(read)read.lba = 1;
    Command copy = {.opcode = 0, .§(designated)compare = {.nlb = 2}};
    return command.§(compare)compare.nlb + copy.read.lba;
}
