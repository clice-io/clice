// Members of class templates over placeholder parameters: the parameters'
// `auto` stays undeduced, and the template heads spell it as written.

template <typename T>
concept Small = sizeof(T) <= 4;

template <auto V>
struct Plain {
    void §(plain)f();
};

template <Small auto V>
struct Constrained {
    void §(constrained)g();
};

template <auto... Vs>
struct Pack {
    void §(pack)h();
};

template <auto V>
struct Split {
    void a();
    void b();
};

template <auto V>
void Split<V>::a() {
    §(body)int x = 0;
}
