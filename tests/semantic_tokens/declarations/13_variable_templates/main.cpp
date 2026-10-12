// Variable template declarations, definitions and specializations receive
// variable tokens.
template <typename T, typename U>
extern int §(pair_value_decl)pair_value;

template <typename T, typename U>
int §(pair_value_def)pair_value = 2;

template <typename T>
extern int §(pair_value_decl2)pair_value<T, int>;

template <typename T>
int §(pair_value_def2)pair_value<T, int> = 4;

template <>
int §(pair_value_def3)pair_value<int, int> = 5;
