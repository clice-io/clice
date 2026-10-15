// A static call or subscript operator named through its class: the name is
// written, so it has the operator's card.

struct Table {
    static int operator()(int row);
    static int operator[](int row);
};

int row = Table::§(qualified_call)operator()(1);
int cell = Table::§(qualified_subscript)operator[](2);
int called = Table{}§(call)(3);
