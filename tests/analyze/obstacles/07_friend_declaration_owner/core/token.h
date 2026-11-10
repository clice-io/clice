#pragma once
struct Token {
    friend bool same(Token, Token);
    int value = 0;
};
