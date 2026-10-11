namespace io {

struct Stream {};

void process(Stream& stream);
void process_all();

namespace detail {
void processor();
}  // namespace detail

}  // namespace io

int processed;
