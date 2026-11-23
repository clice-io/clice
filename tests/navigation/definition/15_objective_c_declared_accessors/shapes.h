__attribute__((objc_root_class))
@interface Object
@end

@protocol Sized
@property(nonatomic) int size;
- (int)§(declaration)size;
@end

@protocol Counted
@property(nonatomic) int count;
@end

@interface Box : Object <Sized, Counted>
- (int)§(class_declaration)count;
@end
