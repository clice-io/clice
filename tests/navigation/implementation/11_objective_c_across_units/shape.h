__attribute__((objc_root_class))
@interface Object
@end

@protocol Sized
@property(nonatomic) int size;
- (int)§(protocol)size;
@end

@interface Shape : Object <Sized>
@end

@interface Pen : Object
@end
