template <class T>
struct §(tpl)Box {
    void a();
    void b();
};

template <class T>
void Box<T>::b() {
    int second = 2;
}

template <class T>
void Box<T>::a() {
    int first = 1;
}
