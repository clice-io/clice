/// # Access and storage indicators
///
/// - status: unsupported
/// - issues: clangd#2123
///
/// Outline entries do not expose access, static, virtual or abstract modifiers
/// yet

class Base {
public:
    virtual void render() = 0;

protected:
    static int instances();

private:
    int id;
};
