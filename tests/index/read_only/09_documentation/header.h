#pragma once

/// Computes the area of a rectangle.
/// @param width The horizontal extent.
/// @return The product of both sides.
int area(int width, int height);

struct Shape {
    int sides; ///< How many sides it has.

    /// Draws the shape.
    virtual void draw();
};

struct Square : Shape {
    void draw() override;
};

/// Makes a copy of a value.
template <typename T, typename = decltype(T{})>
T copy(T value);

#define LIMIT 4 ///< The most it takes.

extern int calls; ///< How many calls so far.

/// The global helper.
void helper();

/// Takes an integer.
void pick(int value);

/// Takes a floating-point number.
void pick(double value);

namespace other {
/// The other target.
void target();

/// @copydoc target
void copied();
}  // namespace other

namespace inner {
/// The inner helper.
void helper();

/// @copydoc ::helper
void absolute();

/// @copydoc pick(double)
void overload(double value);

/// @copydoc other::copied
void nested();
}  // namespace inner
