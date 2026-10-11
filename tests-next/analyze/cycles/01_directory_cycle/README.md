# Directory cycles

Modules whose interfaces name each other form a cycle, listed with the lightest edges whose removal breaks it

Here util names two of core's entities and core one of util's, so the edge
from core to util is the one to cut. core's source calls into feature, and
feature's header names core's Logger: a source makes an implementation
edge, which closes no cycle.
