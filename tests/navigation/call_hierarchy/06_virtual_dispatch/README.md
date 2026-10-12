# Follow virtual dispatch

Incoming calls of a base virtual method do not include calls made through
derived overrides; a call to an override is attributed only to that
override, never to the base it overrides
