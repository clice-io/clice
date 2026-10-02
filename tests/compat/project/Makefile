# Plain make, captured with bear: every flag comes from the scenario.
CC ?= cc
CXX ?= c++
FLAGS := -Iinclude -DCOMPAT_DEFINE=1

all: build/main.o build/util.o

build/main.o: src/main.cpp
	mkdir -p build
	$(CXX) $(FLAGS) $(CXXFLAGS) -c $< -o $@

build/util.o: src/util.c
	mkdir -p build
	$(CC) $(FLAGS) $(CFLAGS) -c $< -o $@
