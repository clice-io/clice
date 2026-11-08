#pragma once

struct command {
    union {
        struct read {
            unsigned lba;
        } read;
        struct io {
            unsigned opcode;
            union {
                struct compare {
                    unsigned nlb : 16;
                } compare;
                unsigned raw;
            };
        } io;
    };
};

typedef struct context {
    struct command* §(header_command)command;
} CONTEXT;
