FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive
ENV RPC_USER=user
ENV RPC_PASS=pass
ENV RPC_PORT=3389
ENV RPC_ALLOW_IP=127.0.0.1
ENV TESTNET=0
ENV EXTRAFLAGS=""
ENV STAKING=0

RUN apt update
RUN apt upgrade -y
RUN apt install -y build-essential libtool autotools-dev automake pkg-config libssl-dev libevent-dev bsdmainutils git cmake libboost-all-dev libgmp3-dev libzmq3-dev
RUN apt install -y software-properties-common
RUN apt install -y wget curl

# Install LevelDB
WORKDIR /root/leveldb
RUN wget -N http://download.oracle.com/berkeley-db/db-4.8.30.NC.tar.gz
RUN tar -xvf db-4.8.30.NC.tar.gz
RUN sed -i s/__atomic_compare_exchange/__atomic_compare_exchange_db/g db-4.8.30.NC/dbinc/atomic.h

WORKDIR /root/leveldb/db-4.8.30.NC/build_unix
RUN mkdir -p build
RUN BDB_PREFIX=/usr/local
RUN ../dist/configure --enable-cxx --prefix=$BDB_PREFIX
RUN make
RUN make install

# Install some more dependencies that are required for the build
RUN apt-get install -y libqt5gui5 libqt5core5a libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev protobuf-compiler qrencode

WORKDIR /root/hydra-blockchain
COPY . .

# Build and Install Hydra
RUN ./autogen.sh
RUN ./configure --without-gui BDB_LIBS="-L${BDB_PREFIX}/lib -ldb_cxx-4.8" BDB_CFLAGS="-I${BDB_PREFIX}/include"
RUN make
RUN make install

EXPOSE ${RPC_PORT}
EXPOSE 3338

ENTRYPOINT hydrad -rpcuser="$RPC_USER" \
    -rpcpassword="$RPC_PASS" \
    -rpcport="$RPC_PORT" \
    -rpcallowip="${RPC_ALLOW_IP}" \
    -rpcbind=0.0.0.0 \
    -staking=${STAKING} \
    -listen \
    -server \
    -testnet=${TESTNET} \
    ${EXTRAFLAGS}