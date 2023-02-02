FROM debian:sid as builder

COPY . /usr/src/ezio

RUN apt-get -y update && \
    apt-get install -y \
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
    libspdlog-dev

WORKDIR /usr/src/ezio

RUN cmake . && make && make install

FROM debian:sid

WORKDIR /app

RUN apt-get -y update && \
    apt-get install -y \
    libgrpc29 \
    libgrpc++1.51 \
    libprotobuf32 \
    libtorrent-rasterbar2.0 \
    libspdlog1.10

copy --from=builder /usr/local/sbin/ezio .

CMD ["/app/ezio"]
