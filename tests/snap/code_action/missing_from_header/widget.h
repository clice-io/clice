#pragma once

struct Tag {};

struct Widget {
    explicit Widget(int id);
    static const Tag& tag();
    virtual void draw(int scale = 1);
    void done();
};
