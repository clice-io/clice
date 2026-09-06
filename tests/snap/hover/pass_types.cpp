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

    take_int(§(01_int_value_var)code);
    take_int(§(02_int_value_literal)456);
    borrow_int(§(03_int_ref_var)code);
    view_int(§(04_int_const_ref_var)code);
    view_int(§(05_int_const_ref_literal)456);
    take_int(§(06_int_value_from_ref)code_ref);
    view_int(§(07_int_const_ref_from_ref)code_ref);
    view_int(§(08_int_const_ref_from_const_ref)code_view);

    borrow_message(§(09_base_ref)message);
    view_message(§(10_base_const_ref)message);
    view_message(§(11_base_const_ref_from_const_ref)message_view);
    take_message(§(12_base_value)message);
    take_message(§(13_base_value_from_const_ref)message_view);
    borrow_message(§(14_derived_to_base_ref)alert);
    view_message(§(15_derived_to_base_const_ref)alert);
    take_message(§(16_derived_to_base_value)alert);

    Envelope first(§(17_ctor_base_const_ref)message);
    auto* second = new Envelope(§(18_new_ctor_base_const_ref)message);
    Envelope third(§(19_ctor_int_ref)code);
    Envelope fourth(code, §(20_ctor_int_value)code);

    take_double(§(21_converted_int_var)code);
    take_double(§(22_converted_int_ref)code_ref);
    take_double(§(23_converted_int_const_ref)code_view);
    take_double(§(24_float_literal)456.0);
    take_double(§(25_converted_int_literal)456);
    take_envelope(§(26_converted_custom_from_int)code);
    take_envelope(§(27_converted_custom_from_float)weight);
    take_envelope(§(28_converted_custom_from_base)message);
}
