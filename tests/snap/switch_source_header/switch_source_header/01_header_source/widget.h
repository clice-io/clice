#pragma once

class Widget {
public:
    void draw();
    int area() const;

private:
    int width = 0;
    int height = 0;
};
