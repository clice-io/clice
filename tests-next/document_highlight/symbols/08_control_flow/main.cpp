void drain(int outer, int inner) {
    for (int i = 0; i < outer; i += 1) {
        for (int j = 0; j < inner; j += 1) {
            if (i == j) {
                §(break)break;
            }
            if (j == 0) {
                §(continue)continue;
            }
        }
    }
}
