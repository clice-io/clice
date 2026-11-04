struct Line {};
struct Circle {};
struct Square {};

void trace(Line* line);
void trace(Circle* circle);
void trace(Square* square);

int blend(int base, int tint);
int blend(int base, int tint, int alpha);

namespace draw {

void fill(Line* line);
void fill(Circle* circle);
void fill(Square* square);

template <class T>
void stamp(T* shape);
template <class T>
void stamp(const T& shape, int times);
template <class T, class U>
void stamp(T* shape, U* other);

}  // namespace draw
