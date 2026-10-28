#pragma once

struct Counter {
    int §(member)count = 0;

    void bump() {
        count = count + 1;
    }
};
