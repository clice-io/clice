# Time the scan

Each run scans the build cold, from a fresh load, then warm, over what the cold scan kept

The cold scan is what a server pays at startup: it reads and lexes every file. The warm one is what a reload of the compilation database pays: it reuses the scans of the files whose bytes did not change, at the cost of one status call each. The last line gives the minimum and the median of both.
