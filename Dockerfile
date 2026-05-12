FROM ubuntu:25.10

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY . .
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build

CMD ["./build/run_sim"]