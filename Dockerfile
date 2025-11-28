FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        pkg-config \
        libopencv-dev \
        libopencv-contrib-dev \
        libzmq3-dev \
        libsqlite3-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the entire source tree into the image (CI-style build).
COPY . /app

# Configure & compile during docker build so runtime containers start fast.
RUN cmake -S . -B build -G Ninja && \
    cmake --build build

# Default entrypoint mirrors local dev flow (three processes run together).
CMD ["./scripts/run_all.sh"]
