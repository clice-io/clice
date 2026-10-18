struct Employee {
    int salary;
    int seniority;
};

template <class It, class F>
F for_each(It first, It last, F f) {
    for (; first != last; ++first) {
        f(*first);
    }
    return f;
}

template <class T, class F>
void each(T& value, F f) {
    f(value);
}

void raise(auto& e) {
    e.sa§(function_template);
}

void review(Employee* begin, Employee* end, Employee& one) {
    for_each(begin, end, [](auto& e) { e.sa§(prefix); });
    for_each(begin, end, [](auto& e) { e.§(bare); });
    each(one, [](auto& e) { e.se§(hand_written); });
    each(one, [](auto& e) { e.salary§(whole_name); });
    raise(one);
}
