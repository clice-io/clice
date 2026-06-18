namespace document_highlight_control_flow {

int classify(int value) {
    while(value > 0) {
        switch(value % 4) {
        case 0:
        case 1:
            value -= 1;
            continue;
        case 2:
            break;
        default:
            throw value;
        }

        if(value == 3)
            break;
        return value;
    }
    return 0;
}

}  // namespace document_highlight_control_flow
