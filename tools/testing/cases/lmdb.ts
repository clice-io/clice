/// The leaf pages of an LMDB file, as a test that damages an index database
/// reads them. A page starts with its number, padding, flags (2 for a leaf)
/// and the end of its node pointers, which follow; a node is four 16-bit
/// fields, the low and high halves of its value's size, its flags and its
/// key's size, then the key and the value.

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

/// Every leaf page of `bytes` in file order. The first meta page records
/// the page size past the page header, the magic, the version, the address
/// and the map size.
export function leaves(bytes: Buffer): Leaf[] {
    const size = bytes.readUInt32LE(40);
    const leaves: Leaf[] = [];
    for (let offset = 2 * size; offset + size <= bytes.length; offset += size) {
        if ((bytes.readUInt16LE(offset + 10) & 0x07) !== 0x02) {
            continue;
        }
        const nodes: Node[] = [];
        for (
            let pointer = offset + 16;
            pointer < offset + bytes.readUInt16LE(offset + 12);
            pointer += 2
        ) {
            const node = offset + bytes.readUInt16LE(pointer);
            const keySize = bytes.readUInt16LE(node + 6);
            nodes.push({
                pointer,
                key: bytes.toString("latin1", node + 8, node + 8 + keySize),
                value: node + 8 + keySize,
                size: bytes.readUInt16LE(node) | (bytes.readUInt16LE(node + 2) << 16),
            });
        }
        leaves.push({ offset, nodes });
    }
    return leaves;
}
