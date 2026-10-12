// Pins a suppression block opened and never closed, which clang-tidy reports
// as an error of its own.
// NOLINTBEGIN(modernize-use-nullptr)
int* begun() {
    return 0;
}
