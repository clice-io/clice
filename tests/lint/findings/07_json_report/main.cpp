void configure(bool enabled);

void call() {
    configure(/*disabled=*/true);
    return missing;
}
