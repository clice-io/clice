#pragma once

// Instantiated before span is declared, wrap's friend declaration is the
// first declaration of span a preamble's PCH holds.
template <class It>
class wrap {
    template <class Tp, unsigned long>
    friend class span;
};

inline void wrapped() {
    wrap<char*> instance;
}

template <typename Tp, unsigned long Extent = 0>
class span;

template <typename Tp, unsigned long Extent>
class span {};

typedef union attr attr;

union attr {
    int flags;
};

void reset(int);
void reset(int level = 0);

template <class T>
struct Later;

template <class T = int>
struct Later {};
