# Other encodings

A file saved in another encoding than UTF-8 reads as UTF-8, and clice names it once

Its bytes past ASCII are no UTF-8: the editor shows each as a replacement
mark. When clice loads the project it names the files it found that are not
UTF-8, once; saving them as UTF-8 fixes them.
