/// Something that can be drawn.
@protocol §(protocol)Drawable
- (void)draw;
@optional
- (void)drawInWidth:(int)width height:(int)height;
@end

__attribute__((objc_root_class))
@interface Object
+ (instancetype)alloc;
- (instancetype)init;
@end

@interface NSString : Object
@end

/// A shape on the canvas.
@interface Shape : Object <Drawable> {
    int _generation;
}
@property(nonatomic) int x;
@property(nonatomic, readonly) int area;
@property(class, nonatomic) int count;
/// Moves the shape.
- (void)setX:(int)x y:(int)y;
+ (instancetype)shapeWithX:(int)x;
@end

@interface Shape (Extras)
- (void)wobble;
@end
