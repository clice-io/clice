#pragma once

/// Computes the area of a rectangle.
/// @param width The horizontal extent.
/// @param height The vertical extent.
/// @return The product of both sides.
int area(int width, int height);

/// @copydoc area
int surface(int width, int height);

struct Shape {
    /// Draws the shape.
    virtual void draw();

    int sides; ///< How many sides it has.
};

struct Square : Shape {
    void draw() override;
};
