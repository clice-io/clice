/// The live leaf pages of an LMDB file, as a test that damages an index
/// database reads them: the file also keeps pages earlier transactions
/// freed, so the walk starts at the newest meta page's root. A page starts
/// with its number, padding, flags (1 for a branch, 2 for a leaf) and the
/// end of its node pointers, which follow; a node is four 16-bit fields,
/// then its key, and in a leaf its value. A leaf node's fields are the low
/// and high halves of the value's size, its flags and the key's size; a
/// branch node's first three are its child's page number.

export interface Node {
    /// Where the node's pointer sits in the file.
    pointer: number;
    /// The key, one character per byte.
    key: string;
    /// Where the value starts in the file, and its size; a value on
    /// overflow pages holds their page number instead.
    value: number;
    size: number;
}

export interface Leaf {
    offset: number;
    nodes: Node[];
}

/// Every live leaf page of `bytes`, in key order. A meta page holds, past
/// the page header, the magic and the version, the address and the map
/// size, then two database records of 48 bytes (the page size in the first
/// one's padding, the root page number at its end), then the last page and
/// the transaction id.
export function leaves(bytes: Buffer): Leaf[] {
    const size = bytes.readUInt32LE(40);
    const meta = [0, size].reduce((newest, offset) =>
        bytes.readBigUInt64LE(offset + 144) > bytes.readBigUInt64LE(newest + 144) ? offset : newest,
    );
    const leaves: Leaf[] = [];
    const walk = (page: number): void => {
        const offset = page * size;
        const flags = bytes.readUInt16LE(offset + 10);
        const pointers: number[] = [];
        for (
            let pointer = offset + 16;
            pointer < offset + bytes.readUInt16LE(offset + 12);
            pointer += 2
        ) {
            pointers.push(pointer);
        }
        if ((flags & 0x01) !== 0) {
            for (const pointer of pointers) {
                const node = offset + bytes.readUInt16LE(pointer);
                walk(
                    bytes.readUInt16LE(node) +
                        bytes.readUInt16LE(node + 2) * 0x10000 +
                        bytes.readUInt16LE(node + 4) * 0x100000000,
                );
            }
            return;
        }
        leaves.push({
            offset,
            nodes: pointers.map((pointer) => {
                const node = offset + bytes.readUInt16LE(pointer);
                const keySize = bytes.readUInt16LE(node + 6);
                return {
                    pointer,
                    key: bytes.toString("latin1", node + 8, node + 8 + keySize),
                    value: node + 8 + keySize,
                    size: bytes.readUInt16LE(node) + bytes.readUInt16LE(node + 2) * 0x10000,
                };
            }),
        });
    };
    walk(Number(bytes.readBigUInt64LE(meta + 128)));
    return leaves;
}
