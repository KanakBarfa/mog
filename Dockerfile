# Consumer-fallback doctrine in container form: the portable profile builds
# with the distro's stock toolchain and runs anywhere amd64 runs.
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake ninja-build ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B build -G Ninja -DMOG_PROFILE=portable -DMOG_CXX_STANDARD=23 \
    && cmake --build build -j"$(nproc)" --target mog

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/mog /usr/local/bin/mog
ENTRYPOINT ["mog"]
