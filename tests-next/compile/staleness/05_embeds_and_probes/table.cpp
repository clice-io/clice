constexpr unsigned char table[] = {
#embed "table.bin"
};

static_assert(sizeof(table) == 2, "the table has two entries");
