// C function declarations: without a prototype, with a return type spelled
// through `__typeof__`, with a `register` parameter; pointers to functions
// with a prototype and without one.

int §(no_prototype)legacy();

int (*§(unprototyped_pointer)callback)();
int (*§(prototyped_pointer)handler)(void);

__typeof__(1.0) §(typeof_return)ratio();

void drain(register int §(register_parameter)count);
