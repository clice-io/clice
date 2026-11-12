#§(empty)
#in§(include)
#ifndef DIRECTIVE_NAMES
#el§(conditional)
#endif
#ifdef DIRECTIVE_NAMES
#else
#e§(after_else)
#endif
#el§(after_endif)
# // see§(after_comment)
#if DIRECTIVE_NAMES
#else
#if DIRECTIVE_OTHER
#e§(nested)
#endif
#endif
