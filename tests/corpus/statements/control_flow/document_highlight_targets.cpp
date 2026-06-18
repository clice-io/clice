namespace document_highlight_targets {

int return_and_throw_exit_the_same_function(int x) {
    if(x < 0)
        return -x;
    if(x == 0)
        throw x;
    auto nested = [] {
        return 1;
    };
    return x;
}

int break_and_continue_target_the_same_loop(int n) {
    while(n-- > 0) {
        if(n == 1)
            break;
        if(n == 2)
            continue;
        switch(n) {
            case 3: break;
        }
        for(int i = 0; i < n; ++i) {
            break;
            continue;
        }
    }
    return n;
}

void switch_break_highlights_the_switch_context(int x) {
    switch(x) {
        case 0: break;
        default: break;
    }
}

void case_highlights_only_the_current_switch_case_group(int x) {
    switch(x) {
        case 0:
        case 1: break;
        default: throw x;
    }
}

}  // namespace document_highlight_targets
