// How each argument reaches its parameter, for every pairing of value, reference,
// const reference and rvalue reference with a variable, a reference, a literal, a
// derived object and a conversion.

struct Message {};
struct Alert : Message {};

struct Envelope {
    Envelope() {}
    Envelope(const Message& message) {}
    Envelope(int& code) {}
    Envelope(double weight) {}
    Envelope(int code, int priority) {}
};

void take_int(int value) {}
void borrow_int(int& value) {}
void view_int(const int& value) {}
void move_int(int&& value) {}
void borrow_message(Message& value) {}
void view_message(const Message& value) {}
void take_message(Message value) {}
void take_double(double value) {}
void take_envelope(Envelope value) {}

void exercise_pass_types() {
    int code = 7;
    int& code_ref = code;
    const int& code_view = code;
    Message message;
    const Message& message_view = message;
    Alert alert;
    double weight = 2.5;

    take_int(code);
    take_int(§(int_value_literal)456);
    borrow_int(code);
    view_int(code);
    view_int(§(int_const_ref_literal)456);
    move_int(§(int_rvalue_ref_literal)456);
    take_int(§(int_value_from_ref)code_ref);
    view_int(§(int_const_ref_from_ref)code_ref);
    view_int(§(int_const_ref_from_const_ref)code_view);

    borrow_message(§(base_ref)message);
    view_message(§(base_const_ref)message);
    view_message(§(base_const_ref_from_const_ref)message_view);
    take_message(§(base_value)message);
    take_message(§(base_value_from_const_ref)message_view);
    borrow_message(§(derived_to_base_ref)alert);
    view_message(§(derived_to_base_const_ref)alert);
    take_message(§(derived_to_base_value)alert);

    Envelope first(§(ctor_base_const_ref)message);
    auto* second = new Envelope(§(new_ctor_base_const_ref)message);
    Envelope third(§(ctor_int_ref)code);
    Envelope fourth(code, §(ctor_int_value)code);

    take_double(§(converted_int_var)code);
    take_double(§(converted_int_ref)code_ref);
    take_double(§(converted_int_const_ref)code_view);
    take_double(§(float_literal)456.0);
    take_double(§(converted_int_literal)456);
    take_envelope(§(converted_custom_from_int)code);
    take_envelope(§(converted_custom_from_float)weight);
    take_envelope(§(converted_custom_from_base)message);
}
