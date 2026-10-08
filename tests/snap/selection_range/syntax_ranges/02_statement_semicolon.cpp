/// # Statements and declarations
///
/// - status: supported
///
/// A statement or declaration is selected with the semicolon that ends it,
/// after the part before it
///
/// A local variable steps from its declarator to the declaration statement.
/// A member, a global or a function declaration gains its semicolon the
/// same way; a template's declaration gains it before its `template` head
/// joins.

struct Counter {
    int §(member)count;
    void §(method)bump();
};

int §(global)limit = 10;

template <class T>
T §(templated)twice(T value);

void run(Counter& counter) {
    counter.§(call)bump();
    int §(local)next = counter.count + 1;
    §(ret)return;
}
