#pragma once

struct Shape;

int §(area_decl)area(const Shape& s);

struct Shape {
    int width;
    int height;
};

struct Renderer {
    virtual void §(render_pure)render() = 0;

    virtual ~Renderer() = default;
};

struct GLRenderer : Renderer {
    void §(render_middle)render() override;
};

struct DebugGLRenderer : GLRenderer {
    void render() override;
};

Shape §(make_unit_shape)make_unit_shape();

int area_scale(const Shape& s);
