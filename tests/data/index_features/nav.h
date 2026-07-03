#pragma once

struct Shape;

int area(const Shape& s);

struct Shape {
    int width;
    int height;
};

struct Renderer {
    virtual void render() = 0;

    virtual ~Renderer() = default;
};

struct GLRenderer : Renderer {
    void render() override;
};

struct DebugGLRenderer : GLRenderer {
    void render() override;
};

Shape make_unit_shape();
