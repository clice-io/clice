namespace a {
int value;
}

namespace b {
int value;
}

int use() { return a::value + b::value; }
