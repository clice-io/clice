/// # Variable templates
///
/// - status: supported
///
/// Declarations, definitions, partial and full specializations

template <typename T, typename U>
extern int §pair_value;

template <typename T, typename U>
int §pair_value = 2;

template <typename T>
extern int §pair_value<T, int>;

template <typename T>
int §pair_value<T, int> = 4;

template <>
int §pair_value<int, int> = 5;
