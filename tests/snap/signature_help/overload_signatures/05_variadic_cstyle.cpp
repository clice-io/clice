/// # C-style variadic function
///
/// - status: supported
///
/// Named parameters are listed while the trailing ellipsis is elided from the
/// label

void record(int code, ...);

int main() {
    record(§(pos)0);
}
