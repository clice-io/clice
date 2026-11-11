@protocol Drawable
- (void)draw;
@end

__attribute__((objc_root_class))
@interface Shape <Drawable>
@end

void use(Shape *shape, id<Drawable> drawing) {
    [§(variable)shape draw];
    [§(qualified_id)drawing draw];
    §(class_name)Shape *other = shape;
    id<§(protocol_name)Drawable> any = drawing;
}
