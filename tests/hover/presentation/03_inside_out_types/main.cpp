namespace inside_out {

struct Widget {
    double scale(double factor) const;
    void reset() & noexcept;
    int id;
    void (*hooks[4])(int);
};

struct Holder {
    int Widget::* member;
};

int storage[16];

void (*§(function_pointer)on_exit)(int);
int (&§(array_reference)row)[16] = storage;
char (*§(array_pointer)line)[80];
void (*§(pointer_array)handlers[8])(int, const char*);
int (*§(variadic)print)(const char*, ...);
int (*§(only_variadic)any)(...);
const auto& §(const_array)view = storage;
double (Widget::*§(member_function)scaler)(double) const = &Widget::scale;
void (Widget::*§(qualified_member)resetter)() & noexcept = &Widget::reset;
int Widget::*§(member_data)field = &Widget::id;
void (*(Widget::*§(member_array)table)[4])(int) = &Widget::hooks;
int Widget::* const §(const_member)fixed = &Widget::id;
int Widget::* Holder::*§(member_in_member)chain = &Holder::member;

void (*§(returns_pointer)signal(int, void (*)(int)))(int);

using Table = void (*[8])(int);
Table §(alias)dispatch;

}
