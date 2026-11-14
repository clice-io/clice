#include "db.h"

int mock_calls = 0;

int mock_count() {
    return mock_calls;
}

void Db::open() {
    mock_calls += 1;
}
