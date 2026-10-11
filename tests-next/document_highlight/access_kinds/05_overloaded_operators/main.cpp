// Overloaded =, +=, ++ and -- write their left operand like the built-in
// ones; other operators and member calls read it, explicit object parameters
// included. An operand or argument taken by mutable reference is a write, as
// in a stream extraction. The operator itself highlights at its declaration
// and every use, spelled or not.

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
