# Build and test environment for tests/ci/loopback_queue_depth.sh.
# Same base and build dependencies as the top-level Dockerfile.
FROM debian:sid

RUN apt-get -y update && \
    apt-get install -y --no-install-recommends \
    build-essential \
    libssl-dev \
    cmake \
    make \
    libgrpc-dev \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc \
    libtorrent-rasterbar-dev \
    libboost-program-options-dev \
    libspdlog-dev \
    python3 \
    python3-libtorrent \
    python3-grpcio \
    python3-grpc-tools \
    openssl \
    opentracker \
    procps && \
    rm -rf /var/lib/apt/lists/*
