/// # Symbol Tags
///
/// ## Access and storage indicators — public / private / protected, static, virtual and abstract markers on outline entries
///
/// - status: unsupported
/// - issues: clangd#2123
/// - order: 2

class Base {
public:
    virtual void render() = 0;

protected:
    static int instances();

private:
    int id;
};
