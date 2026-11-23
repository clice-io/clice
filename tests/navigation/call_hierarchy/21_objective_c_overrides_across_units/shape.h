__attribute__((objc_root_class))
@interface Object
@end

@protocol Named
@property(nonatomic) int count;
@end

@interface Shape : Object <Named>
@end

@interface Mid : Shape
@end

@interface Pen : Object <Named>
@end
