/// # Sloppy name matching
///
/// - status: partial
/// - issues: clangd#2248
///
/// `aParam` does not yet suppress an argument spelled `param`

void draw(int aParam);

void use() {
    int param = 3;
    // Ideally the near-match would suppress the hint; today it still shows.
    draw(param);
}
