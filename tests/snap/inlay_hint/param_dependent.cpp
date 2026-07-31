/// # Parameter Hints
///
/// ## Dependent calls — parameter names appear even when the callee is only known inside a template
///
/// - status: supported
/// - order: 9
///
/// Candidates are matched by argument count; only a unique surviving
/// candidate names the parameters, so a call that could still hit several
/// overloads stays bare rather than guessing.

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
        // Several overloads remain viable: no hint.
        overload(T{});
    }
};
