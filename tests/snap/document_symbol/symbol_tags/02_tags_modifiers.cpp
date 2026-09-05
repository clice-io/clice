/// # Access and storage indicators
///
/// - status: unsupported
/// - issues: clangd#2123
///
/// Public / private / protected, static, virtual and abstract markers on outline entries

class Base {
public:
    virtual void render() = 0;

protected:
    static int instances();

private:
    int id;
};
