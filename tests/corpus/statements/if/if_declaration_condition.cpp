// if with declaration as condition
namespace if_declaration {

struct Optional {
    int value;
    bool valid;

    explicit operator bool() const {
        return valid;
    }
};

Optional try_parse(int x) {
    if(x >= 0)
        return {x * 2, true};
    return {0, false};
}

int use_declaration_condition(int x) {
    if(Optional result = try_parse(x)) {
        return result.value;
    }
    return -1;
}

// pointer declaration as condition
struct Node {
    int data;
    Node* next;
};

int walk_list(Node* head) {
    int sum = 0;
    if(Node* p = head) {
        sum += p->data;
    }
    return sum;
}

void test() {
    [[maybe_unused]] int r1 = use_declaration_condition(5);
    [[maybe_unused]] int r2 = use_declaration_condition(-1);
    Node n{42, nullptr};
    [[maybe_unused]] int r3 = walk_list(&n);
}

}  // namespace if_declaration
