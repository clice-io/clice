# Function locals

A function's local variables and parameters get no call hierarchy item

The only function that can use them is the one declaring them, so an item
for them would have one caller, known in advance. A field of a class
declared in a function keeps its item: the class's methods use it.
