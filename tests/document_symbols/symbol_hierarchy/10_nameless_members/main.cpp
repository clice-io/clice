// Nameless declarations name nothing a reader looks for: an unnamed
// bit-field pads, an unnamed catch parameter is never read.

struct Flags {
    int : 3;
    int ready : 1;
};

void guarded() {
    try {
    } catch(const Flags&) {
    } catch(int code) {
    }
}
