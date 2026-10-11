// Overloads reorder by the order of their declarations, and so do the
// members of a class template.

void over(int);
void over(double);
void other();

void over(double) {}
void §(overloads)over(int) {}
void other() {}

template <class T>
struct §(tpl)Box {
    void a();
    void b();
};

template <class T>
void Box<T>::b() {}

template <class T>
void Box<T>::a() {}
