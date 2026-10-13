namespace params {
/// Scales a value.
/// @tparam T The arithmetic type.
/// @param value The value to scale.
/// @param factor How much to scale by.
template <typename T>
§(template_param)T scale(T §(declared)value, T factor) {
    return value * fac§(used)tor;
}

struct Box {
    /// Resizes the box.
    /// @param[in] width The new width.
    void resize(int width);
};

void Box::resize(int §(out_of_line)width) {}
}
