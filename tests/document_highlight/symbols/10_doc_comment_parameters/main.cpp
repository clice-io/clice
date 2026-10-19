/// Clamps a value.
/// @param §(comment)value what to clamp
/// @param[in] limit the most it may be
int clamp(int §(declared)value, int limit) {
    return value < limit ? value : limit;
}
