#define DEFAULT_TIMEOUT 30

namespace function_signature {

int §(params)add(int lhs, int rhs);

void §(defaults)configure(int width, bool visible = true);

void §(written_defaults)open_file(int mode = 0644, int timeout = DEFAULT_TIMEOUT);

}
