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
