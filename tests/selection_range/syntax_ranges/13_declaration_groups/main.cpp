int §(first)width = 640, height = 480;

int §(attributed)low, high [[maybe_unused]];

struct Size {
    int §(member)rows, columns;
};

int total(Size size) {
    // Halves.
    auto [§(binding)top, bottom_half_of_the_size] = size;
    return top + bottom_half_of_the_size;
}
