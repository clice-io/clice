int classify(int code) {
    int result = 0;
    switch (code) {
        case 1:
            result += 1;
            result *= 2;
            break;
        case 2:
        case 3:
            result -= 1;
            result *= 3;
            break;
        case 4: {
            int scratch = code;
            result = scratch;
            break;
        }
        case 5: result = 5;
            result += 1;
            break;
        case 6: {
            int scratch = code;
            result = scratch;
        }
            result += 6;
            break;
        default:
            result = -1;
            break;
    }
    return result;
}
