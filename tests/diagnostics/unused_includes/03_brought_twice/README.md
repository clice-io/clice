# Brought by another include

An include whose headers another include brings too is reported

When two includes bring the same headers and the file uses nothing else of the second, the second is reported: the reported ones can all go at once.
