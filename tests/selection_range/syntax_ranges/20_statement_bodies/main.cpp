int classify(int code) {
    switch(code) {
    case 0:
        return §(case_body)code;
    [[likely]] case 1:
        code += §(attributed)1;
        break;
    default:
        break;
    }
    if(code > 2) return §(if_body)code;
retry:
    code -= §(labeled)1;
    if(code > 0) goto retry;
    return code;
}
