template <typename T>
void apply(T scale);

template <typename T>
struct Holder {
    void member(T item);
    static void static_member(T slot);
};

void overload(int value);
void overload(double value);

template <typename T>
struct Runner {
    void run(Holder<T> holder, T value) {
        apply(value);
        holder.member(value);
        Holder<T>::static_member(value);
        overload(T{});
    }
};
