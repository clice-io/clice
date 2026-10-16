// A block a macro closes folds up to the macro, not to a later brace.
#define END }

void check(bool ready) {
    if (ready) {
        ready = false;
        ready = !ready;
    END
    ready = true;
    ready = !ready;
}
