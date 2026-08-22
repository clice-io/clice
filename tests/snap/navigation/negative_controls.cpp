/// - verify: server

int choose_literal(bool condition) {
    §(keyword_if)if (condition)
        §(keyword_return)return §(literal_value)42;
    return 0;
}
