#pragma once

struct Tag {};

struct Widget {
    explicit Widget(int id);
    static Tag tag();
    static const Tag& last();
    virtual void draw(int scale = 1);
    void done();
};
