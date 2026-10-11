# Module partitions

A module split into partitions gives its importers what every partition its primary interface re-exports declares

A partition imports a sibling by its partition name alone, `import :Types;`,
and the primary interface uses what its partitions declare.
