/// # Export status hover
///
/// - status: supported
///
/// The card of a module's declaration shows whether the module exports it
///
/// An exported declaration's definition reads `export`, whether exported
/// on its own or in an `export` block; a declaration the module keeps to
/// itself, or a member of an exported class, does not.

export module shapes;

export int §(exported_fn)area(int side) {
    return side * side;
}

export {
    struct §(exported_struct)Square {
        int §(member)side;
    };
}

int §(internal_fn)perimeter(int side) {
    return side * 4;
}
