// - verify: server
//
// Every token of a name written with several — a destructor's `~` and class
// name, `operator` and its symbol — stands for the function, and its rows
// span the whole name. The type a conversion function's name embeds stays
// the type's.

struct Handle {};

template <typename T>
struct §(box)Box {
    §(dtor_tilde)~§(dtor_class)Box();
};

template <typename T>
Box<T>::~Box() {}

struct Vec {
    int x;
    bool §(eq_keyword)operator§(eq_symbol)==(const Vec& other) const;
    int operator§(call_paren)()(int index) const;
    §(conv_keyword)operator §(conv_type)Handle*() const;
};

bool Vec::operator==(const Vec& other) const {
    return x == other.x;
}

int Vec::operator()(int index) const {
    return x + index;
}

Vec::operator Handle*() const {
    return nullptr;
}

unsigned long long operator""§(literal_suffix)_km(unsigned long long value) {
    return value * 1000;
}

bool use(Box<int>* box, const Vec& left, const Vec& right) {
    box->~§(dtor_call)Box<int>();
    bool (Vec::*equal)(const Vec&) const = &Vec::§(eq_pointer)operator==;
    Handle* handle = left.operator §(conv_call_type)Handle*();
    unsigned long long distance = 5_km;
    return left §(eq_use)== right && left.§(eq_explicit)operator==(right) && (left.*equal)(right) &&
           left(1) == 2 && handle == nullptr && distance > 0;
}
