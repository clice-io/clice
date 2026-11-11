__attribute__((objc_root_class))
@interface Shape
@property int width;
+ (instancetype)shapeWithWidth:(int)width;
+ (Shape *)unit;
- (void)moveToX:(int)x;
- (void)moveToX:(int)x y:(int)y;
- (int)area;
@end

@interface Square : Shape
@end

void use(Shape *shape) {
    [shape mo§(message)];
    [shape ar§(unary)];
    [shape moveToX:1 y§(next_piece)];
    [shape mo§(written):1];
    [Shape sha§(class_message)];
    int area = shape.ar§(method_as_property);
    Shape *unit = Shape.un§(class_method_as_property);
    SEL moving = @selector(mo§(selector));
    SEL renamed = @selector(mo§(selector_written):y:);
}

@implementation Square
- (void)moveToX:(int)x y:(int)y {
    [super mo§(super_message)];
    [super mo§(super_written):x y:y];
}
- (void)mo§(declaration) {
}
- (void)moveToX:(int)x y§(declaration_piece) {
}
- ar§(untyped_declaration) {
}
@end
