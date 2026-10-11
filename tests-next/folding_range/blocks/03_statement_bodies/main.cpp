void process(int count) {
    if (count > 0) {
        for (int i = 0; i < count; i += 1) {
            count -= 1;
        }
    }

    while (count > 0) {
        count -= 1;
    }

    {
        int scratch = count;
        count = scratch + 1;
    }

    if (count > 10) {
        count += 1;
    } else {
        count -= 1;
    }

    try {
        count *= 2;
    } catch (...) {
        count = 0;
    }

    do {
        count -= 1;
    } while (count > 10);
}
