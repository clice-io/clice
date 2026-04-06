#ifdef CONFIG_A
int config_value() {
    return 1;
}
#endif

#ifdef CONFIG_B
int config_value() {
    return 2;
}
#endif

int main() {
    return config_value();
}
