/// # `import :partition` navigates to the partition unit
///
/// - status: supported
/// - verify: server
///
/// Go-to-definition on the partition name after the colon in a partition
/// import opens the partition unit that declares it.

import pack;

int run() {
    return §(use)count();
}
