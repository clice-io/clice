/// # Writes through overloaded operators
///
/// - status: supported
///
/// The overloaded assignment, compound assignment, increment and decrement
/// operators of a class write their left operand like the built-in ones
///
/// Other operators read it, and so does a call of a member function, an
/// explicit object parameter's included. An operand or argument is a write
/// when the operator or member takes it by mutable reference, as a stream
/// extraction does.
/// The operator itself highlights where it is declared and wherever an
/// expression uses it.

struct Stream {};

Stream& operator>>(Stream& in, int& value);

struct Meter {
    Meter& operator=(int value);
    Meter& operator+=(int value);
    Meter& operator++();
    Meter operator--(int);
    Meter §(plus_decl)operator+(int value) const;
    Meter& operator-=(this Meter& self, int value);
    void operator()(this Meter& self, int& sink);
    void bump(this Meter& self, int& by);
};

void measure(Stream& §(stream)in) {
    Meter §(meter)meter;
    int §(reading)reading = 0;
    in >> reading;
    meter = reading;
    meter += 1;
    ++meter;
    meter--;
    Meter total = meter §(plus_use)+ 2;
    Meter more = meter.operator+(3);
    meter -= 1;
    meter(reading);
    meter.bump(reading);
}
