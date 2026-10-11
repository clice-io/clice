namespace detail {

struct Widget {
    int width;
    int height;
§(struct_end)⟦};
⟧
int area(const Widget& widget) {
    int total = 0;
    while (total < widget.width) {
        total += widget.height;
    §(while_end)⟦}
⟧    if (total > 10) {
        total = 10;
    } else if (total < 0) {
        total = 0;
    §(else_if_end)⟦}
⟧    if (widget.width > widget.height && widget.height > 0 && total != widget.width) {
        total += 1;
    §(long_condition_end)⟦}
⟧    return total;
§(function_end)⟦}
⟧
int tiny() { return 0; §(one_line_end)⟦}
⟧
int a_function_whose_name_alone_is_long_enough_to_pass_sixty_chars() {
    return 0;
§(long_name_end)⟦}
⟧
§(namespace_end)⟦}
⟧
