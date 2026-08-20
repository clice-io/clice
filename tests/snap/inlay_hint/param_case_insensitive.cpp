/// # Parameter Hints
///
/// ## Sloppy name matching — `aParam` does not yet suppress an argument spelled `param`
///
/// - status: partial
/// - issues: clangd#2248
/// - order: 15

void draw(int aParam);

void use() {
    int param = 3;
    // Ideally the near-match would suppress the hint; today it still shows.
    draw(param);
}
