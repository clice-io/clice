/// # Module fragments and export blocks
///
/// - status: supported
///
/// The global and private module fragments and `export` blocks form folding
/// ranges

module;

#define GMF_A 1
#define GMF_B 2

export module demo;

export {
    int first(int value) {
        return value;
    }
    int second(int value);
}

export namespace ns {
    int third(int value);
}

module :private;

int second(int value) {
    return value;
}
