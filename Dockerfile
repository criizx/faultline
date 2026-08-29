FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends cmake g++ ninja-build && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFAULTLINE_BUILD_TESTS=OFF && cmake --build build

FROM ubuntu:24.04

RUN useradd --create-home --uid 10001 faultline
COPY --from=builder /source/build/faultline /usr/local/bin/faultline
USER faultline
ENTRYPOINT ["faultline"]
