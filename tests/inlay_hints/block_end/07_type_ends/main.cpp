struct Plain {
};

class Kind {
};

union Either {
};

enum Mode {
};

enum class Level {
};

struct Spaced {
}  ;

struct Split {
}

;

struct Doubled {
};;

void function_with_semicolon() {
};

struct {
    int x;
}

anonymous;

struct Wide {
}      ;

struct Commented {
}; /* Put anything here */

struct Outer {
    struct Inner {
    };};

#define DECL_STRUCT(NAME) struct NAME {
#define RBRACE }

DECL_STRUCT(FromMacro)
};

DECL_STRUCT(BraceFromMacro)
RBRACE;
