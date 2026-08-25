/// # Type Information
///
/// ## Variable types — pointers, references, arrays
///
/// - status: supported
/// - order: 1
///
/// A variable's card pretty-prints its declared type, spelling the pointer,
/// reference and array declarators the way they read in source.

namespace variable_type {

int target;

int *§(01_pointer)ptr = &target;

int &§(02_reference)ref = target;

int §(03_array)numbers[4]{};

}
