#pragma once

struct Conv {
    operator int() const;
    int get() const;
};

struct Base {
    virtual int run() const;
};
