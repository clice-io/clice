namespace members {

enum Col§(enum_type)or {
    Red,
    Green = 4,
    Blue,
};

struct Poi§(struct_type)nt {
    using Scalar = double;
    Scalar x;
    Scalar y;
    Scalar length() const;
};

}
