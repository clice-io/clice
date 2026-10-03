// The `#pragma clang __debug` commands that crash, hang or dump the compiler
// do nothing: the code around them highlights as usual.

int before;
#pragma clang __debug crash
#pragma clang __debug parser_crash
#pragma clang __debug assert
#pragma clang __debug llvm_unreachable
#pragma clang __debug llvm_fatal_error
#pragma clang __debug overflow_stack
#pragma clang __debug dump before
int after;
