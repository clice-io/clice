/// # Nested calls
///
/// - status: supported
///
/// The inner call's help shows at the inner marker and the outer call's help at the outer marker

int inner(int a);
int outer(int b, int c);

int main() {
    outer(inner(§(deep)1), §(shallow)2);
}
