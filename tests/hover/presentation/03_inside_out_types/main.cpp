namespace inside_out {

struct Widget {
    double scale(double factor) const;
    void reset() & noexcept;
    int id;
    void (*hooks[4])(int);
};

int storage[16];

void (*§(function_pointer)on_exit)(int);
int (&§(array_reference)row)[16] = storage;
char (*§(array_pointer)line)[80];
void (*§(pointer_array)handlers[8])(int, const char*);
int (*§(variadic)print)(const char*, ...);
double (Widget::*§(member_function)scaler)(double) const = &Widget::scale;
void (Widget::*§(qualified_member)resetter)() & noexcept = &Widget::reset;
int Widget::*§(member_data)field = &Widget::id;
void (*(Widget::*§(member_array)table)[4])(int) = &Widget::hooks;

void (*§(returns_pointer)signal(int, void (*)(int)))(int);

using Table = void (*[8])(int);
Table §(alias)dispatch;

}
