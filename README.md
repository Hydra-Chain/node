# What is HYDRA?

HYDRA is a decentralized open source bookings ecosystem for renting hotel rooms, private properties or accommodation. HYDRA will be the first system which allows end customers and property owners to deal with each other on the platform without any fee or commission.

The HYDRA Blockchain is a hybrid utilizing the transaction model of Bitcoin and employing the powerful virtual machine of Ethereum. It is based on the Bitcoin Core, Ethereum and Qtum.
It features a Proof of Stake consensus mechanism, high transaction throughput, democratic governance of key parameters, predictable network fees and a unique profit-sharing system.

# License
HYDRA is [GPLv3 licensed](https://www.gnu.org/licenses/gpl-3.0.html)

# Resources
Mainnet explorer: http://explorer.hydrachain.org/
Testnet explorer: http://testexplorer.hydrachain.org/
Testnet faucet: http://faucet.hydrachain.org
Docs: https://docs.hydrachain.org/
Wallet downloads: https://github.com/Hydra-Chain/node/releases

# Building HYDRA Blockchain

### Build on Ubuntu

    # This is a quick start script for compiling HYDRA on  Ubuntu

    sudo apt-get install build-essential libtool autotools-dev automake pkg-config libssl-dev libevent-dev bsdmainutils git cmake libboost-all-dev libgmp3-dev
    sudo apt-get install software-properties-common
    sudo add-apt-repository ppa:bitcoin/bitcoin
    sudo apt-get update
    sudo apt-get install libdb4.8-dev libdb4.8++-dev

    # If you want to build the Qt GUI:
    sudo apt-get install libqt5gui5 libqt5core5a libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev protobuf-compiler qrencode

    git clone https://github.com/Hydra-Chain/node.git --recursive HYDRA
    cd HYDRA

    # Note autogen will prompt to install some more dependencies if needed
    ./autogen.sh
    ./configure 
    make -j2
    
### Build on CentOS

Here is a brief description for compiling HYDRA on CentOS

    # Compiling boost manually
    sudo yum install python-devel bzip2-devel
    git clone https://github.com/boostorg/boost.git
    cd boost
    git checkout boost-1.66.0
    git submodule update --init --recursive
    ./bootstrap.sh --prefix=/usr --libdir=/usr/lib64
    ./b2 headers
    sudo ./b2 -j4 install
    
    # Installing Dependencies for HYDRA
    sudo yum install epel-release
    sudo yum install libtool libdb4-cxx-devel openssl-devel libevent-devel gmp-devel
    
    # If you want to build the Qt GUI:
    sudo yum install qt5-qttools-devel protobuf-devel qrencode-devel
    
    # Building HYDRA
    git clone https://github.com/Hydra-Chain/node.git --recursive HYDRA
    cd HYDRA
    ./autogen.sh
    ./configure
    make -j4

### Build on OSX

The commands in this guide should be executed in a Terminal application.
The built-in one is located in `/Applications/Utilities/Terminal.app`.

#### Preparation

Install the OS X command line tools:

`xcode-select --install`

When the popup appears, click `Install`.

Then install [Homebrew](https://brew.sh).

#### Dependencies

    brew install cmake automake berkeley-db4 libtool boost miniupnpc openssl pkg-config protobuf qt5 libevent imagemagick librsvg qrencode gmp

NOTE: Building with Qt4 is still supported, however, could result in a broken UI. Building with Qt5 is recommended.

#### Build HYDRA 

1. Clone the HYDRA source code and cd into `HYDRA`

        git clone https://github.com/Hydra-Chain/node.git --recursive HYDRA
        cd HYDRA

2.  Build HYDRA:

    Configure and build the headless HYDRA binaries as well as the GUI (if Qt is found).

    You can disable the GUI build by passing `--without-gui` to configure.

        ./autogen.sh
        ./configure
        make

3.  It is recommended to build and run the unit tests:

        make check

### Run

Then you can either run the command-line daemon using `src/hydrad` and `src/hydra-cli`, or you can run the Qt GUI using `src/qt/hydra-qt`