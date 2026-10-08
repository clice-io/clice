// A parameter named in characters past ASCII: the label's parameter offsets
// count UTF-16 units, so the active parameter's brackets land on it.

void greet(int 参数, double second);

int main() {
    greet(1, §(pos)2.0);
}
