struct S {
    int field;

    int compute() {
        int local = 1;
        auto by_copy = [§(local_use)local, §(this)this] {
            return §(local_use2)local + this->§(field)field;
        };
        auto by_reference = [&§(local_use3)local] {
            return §(local_use4)local;
        };
        return by_copy() + by_reference();
    }
};
