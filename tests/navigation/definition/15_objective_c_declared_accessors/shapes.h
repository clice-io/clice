__attribute__((objc_root_class))
@interface Object
@end

@protocol Sized
@property(nonatomic) int size;
- (int)§(declaration)size;
@end

@interface Box : Object <Sized>
@end
