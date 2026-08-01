/// # Fold Kinds
///
/// ## Macro-generated folding — braces and access specifiers spelled through macros fold at the invocation site
///
/// - status: supported
/// - order: 15

#define NS_BEGIN namespace ns {
#define NS_END }
#define PUBLIC public:
#define PRIVATE private:

NS_BEGIN

class Widget {
PUBLIC
    void draw();
    void resize();
PRIVATE
    int width;
    int height;
};

NS_END
