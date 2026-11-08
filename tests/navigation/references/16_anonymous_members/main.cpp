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

int submit(Command& command) {
    command.§(read)read.lba = 1;
    Command copy = {.opcode = 0, .§(designated)compare = {.nlb = 2}};
    return command.§(compare)compare.nlb + copy.read.lba;
}
