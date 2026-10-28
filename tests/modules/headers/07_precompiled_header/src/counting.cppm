export module counting;
export import :part;

export int twice() {
    pch::Counter counter;
    §(interface)step(counter);
    return counter.next();
}
