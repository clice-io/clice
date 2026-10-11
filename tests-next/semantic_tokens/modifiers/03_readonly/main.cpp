enum class Level { §(High)High };

const int §(limit_def)limit = 10;
constexpr int §(bound)bound = 4;

struct Gauge {
    int §(read_decl)read() const;
    void §(write_decl)write(int value);
};

void probe(const int& §(in)in, const int* §(pointee_const)pointee_const, int* const §(self_const)self_const) {
    Gauge gauge;
    gauge.§(read_use)read();
    gauge.§(write_use)write(§(limit_use)limit);
}
