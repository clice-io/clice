template <typename §(T_def)T, int §(N_def)N>
struct §(Array_def)Array {
    §(T_use)T data[§(N_use)N];
};

template <typename T>
T §(identity_decl)identity(T value);

template <typename §(T_def2)T>
§(T_use2)T §(identity_def)identity(§(T_use3)T value) {
    return value;
}

§(Array_use)Array<int, 4> arr;
int result = §(identity_use)identity(3);
