/// # Declarations & References
///
/// ## Variable templates — declarations, definitions, partial and full specializations
///
/// - status: supported
/// - order: 17

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
