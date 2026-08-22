/// - verify: server

template <typename T>
T §(primary)zero = T{};

template <>
int §(specialization)zero<int> = 7;

int read_zero() {
    return §(spec_use)zero<int> + static_cast<int>(§(primary_use)zero<double>);
}
