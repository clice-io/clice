/// Counts the items.
template <typename T>
int count(const T& items) {
    int total = 0;
    for(auto& item: items) {
        total += 1;
    }
    return total; // done
}

#if 0
int disabled = 1;
#endif
