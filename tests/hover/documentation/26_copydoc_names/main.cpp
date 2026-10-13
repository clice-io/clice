/// The global helper.
void helper();

namespace names {
struct Base {
    /// Draws the shape.
    /// @param scale How much larger.
    /// @return Whether it drew.
    bool draw(double scale);
};

template <typename T>
struct Holder {
    /// Adds an element.
    void push(T value);
};

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

/// @copydoc Base::draw
bool §(qualified)qualified(double scale);

/// @copydoc Base#draw()
bool §(hash)hash(double scale);

/// @copydoc Holder::push
void §(template_member)template_member(int value);

/// @copydoc pick(double)
void §(overload)overload(double value);

/// @copydoc pick(long)
void §(no_overload)no_overload(long value);

/// @copydoc ::helper
void §(absolute)absolute();

/// @copydoc helper
void §(relative)relative();

/// @copydoc other::copied
void §(nested)nested();

/// Draws twice as large.
///
/// @copydetails Base::draw
bool §(details)details(double scale);
}  // namespace inner
}  // namespace names
