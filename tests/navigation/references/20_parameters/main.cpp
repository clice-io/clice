int clamp(int §(value)value, int §(limit)limit) {
    if (value > limit) {
        return limit;
    }
    return value < 0 ? 0 : value;
}
