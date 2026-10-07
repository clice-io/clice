# Plain make, captured with bear: the compilers and their flags come from
# the scenario's command line.
FLAGS := -Iinclude -DCOMPAT_DEFINE=1

all: build/main.o build/util.o

build/main.o: src/main.cpp
	mkdir -p build
	$(CXX) $(FLAGS) $(CXXFLAGS) -c $< -o $@

build/util.o: src/util.c
	mkdir -p build
	$(CC) $(FLAGS) $(CFLAGS) -c $< -o $@

# CUDA through nvcc: a device source and a host-language source.
cuda: build/kernel.o build/host.o

build/kernel.o: cuda/kernel.cu
	mkdir -p build
	$(NVCC) $(FLAGS) $(NVCCFLAGS) -c $< -o $@

build/host.o: cuda/host.cpp
	mkdir -p build
	$(NVCC) $(FLAGS) $(NVCCFLAGS) -c $< -o $@
