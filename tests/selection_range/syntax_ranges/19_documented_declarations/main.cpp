/// The largest size a §(comment)buffer takes.
constexpr int limit = 64;

struct Buffer {
    int §(member)used = 0; ///< Bytes in use.
};

/**
 * Copies the §(line)bytes over.
 * Returns how many it copied.
 */
int copy(Buffer& to, const Buffer& §(parameter)from);

int spare(const Buffer& buffer) {
    /// Bytes still free.
    int §(local)left = limit - buffer.used;
    return left;
}
