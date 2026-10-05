/// # Forwarding references and declarator types
///
/// - status: supported
///
/// `auto&&` bound to an lvalue takes the deduced reference in place of both tokens, and a type that wraps the name is left alone

int value;
int fn(int);

void f(int (&array)[3]) {
    §(lvalue)auto&& ref = value;
    §(rvalue)auto&& temp = 1;
    §(pointer)auto callback = &fn;
    §(array)auto& bound = array;
}
