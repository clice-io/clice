template <typename T>
void apply(T scale);

template <typename T>
struct Holder {
    void member(T item);
    static void static_member(T slot);
};

void overload(int value);
void overload(double value);

void place(int x, int y);
void place(const char* name, int y);

template <typename T>
struct Runner {
    void run(Holder<T> holder, T value) {
        apply(value);
        holder.member(value);
        Holder<T>::static_member(value);
        overload(T{});
        place(1, value);
    }
};
