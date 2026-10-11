struct Value {
    Value& §(operator_use)operator=(const Value& other);
    Value §(operator_use2)operator+(const Value& other) const;
};

void combine(Value a, Value b) {
    a §(assign)= b;
    Value c = a §(plus)+ b;
}
