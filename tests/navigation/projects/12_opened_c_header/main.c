#include "command.h"

CONTEXT current;

void reset(void) {
    current.§(command)command->§(io)io.§(compare)compare.§(nlb)nlb = 1;
    current.command->§(read)read.lba = 0;
}
