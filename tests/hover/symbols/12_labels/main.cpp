// A label has a card at the goto that jumps to it and where it stands.

namespace jump_target {
int adjust(int value) {
    if (value < 0) {
        goto §(goto_label)finish;
    }
    value *= 2;
§(label_def)finish:
    return value;
}
}
