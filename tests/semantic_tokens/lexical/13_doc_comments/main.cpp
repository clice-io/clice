/// Scales a value.
/// \param value the value to scale
/// @param factor how much, at least 1
/// @return the scaled value
int scale(int value, int factor);

/**
 * @brief A point on the plane.
 * @tparam T the coordinate type
 */
template <class T>
struct Point {
    T x, y;
};

int width; ///<@brief How wide.

///@brief Clamps a value.
/// Kept within @f$ [0, 1] @f$.
int clamp(int value);

// @param plain comments have no commands
