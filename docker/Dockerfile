
FROM debian:13 AS builder

# Installs System Dependencies
RUN apt-get update && apt-get install -y \
    ninja-build cmake build-essential curl gcc-14 g++-14 git

# Installs LLVM 20
RUN curl https://apt.llvm.org/llvm-snapshot.gpg.key | tee /usr/share/keyrings/llvm-snapshot.gpg.key && \
    echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg.key] http://apt.llvm.org/trixie/ llvm-toolchain-trixie main" >> /etc/apt/sources.list && \
    echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg.key] http://apt.llvm.org/trixie/ llvm-toolchain-trixie-20 main" >> /etc/apt/sources.list && \
    apt-get update && apt-get install -y \
    clang-20 lld-20
RUN ln -s /usr/bin/lld-20 /usr/bin/lld

# Adds source code
COPY include /app/include
COPY cmake /app/cmake
COPY src /app/src
COPY tests /app/tests
COPY CMakeLists.txt /app/CMakeLists.txt
COPY scripts /app/scripts

WORKDIR /app
# Initializes and installs dependencies
RUN cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_BUILD_TYPE=Release -DCLICE_ENABLE_TEST=ON
# Builds clice
RUN cmake --build build -j && \
    cmake --install build --prefix=/opt/clice

FROM debian:13

COPY --from=builder /opt/clice /opt/clice/
COPY LICENSE /opt/clice/LICENSE
COPY README.md /opt/clice/README.md

RUN ln -s /opt/clice/bin/clice /usr/local/bin/clice

ENTRYPOINT ["clice"]
