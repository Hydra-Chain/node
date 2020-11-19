HYDRA
========

Setup
---------------------
HYDRA is the original HYDRA Blockchain client and it builds the backbone of the network. It downloads and, by default, stores the entire history of HYDRA transactions; depending on the speed of your computer and network connection, the synchronization process can take anywhere from a few hours to a day or more.

To download HYDRA, visit [the download page](https://github.com/Hydra-Chain/node/releases).

Running
---------------------
The following are some helpful notes on how to run HYDRA on your native platform.

### Unix

Unpack the files into a directory and run:

- `bin/hydra-qt` (GUI) or
- `bin/hydrad` (headless)

### Windows

Unpack the files into a directory, and then run hydra-qt.exe.

### OS X

Drag HYDRA to your applications folder, and then run HYDRA.

### Need Help?

<!--* See the documentation at the [Bitcoin Wiki](https://en.bitcoin.it/wiki/Main_Page)-->
<!--for help and more information.-->
* Ask for help on the [issues page](https://github.com/Hydra-Chain/node/issues).

Building
---------------------
The following are developer notes on how to build HYDRA on your native platform. They are not complete guides, but include notes on the necessary libraries, compile flags, etc.

- [OS X Build Notes](build-osx.md)
- [Unix Build Notes](build-unix.md)
- [Windows Build Notes](build-windows.md)
- [Gitian Building Guide](gitian-building.md)

Development
---------------------
The HYDRA repo's [root README](/README.md) contains relevant information on the development process and automated testing.

- [Developer Notes](developer-notes.md)
- [Release Notes](release-notes.md)
- [Release Process](release-process.md)
- [Translation Process](translation_process.md)
- [Translation Strings Policy](translation_strings_policy.md)
- [Travis CI](travis-ci.md)
- [Unauthenticated REST Interface](REST-interface.md)
- [Shared Libraries](shared-libraries.md)
- [BIPS](bips.md)
- [Dnsseed Policy](dnsseed-policy.md)
- [Benchmarking](benchmarking.md)

### Miscellaneous
- [Assets Attribution](assets-attribution.md)
- [Files](files.md)
- [Fuzz-testing](fuzzing.md)
- [Reduce Traffic](reduce-traffic.md)
- [ZMQ](zmq.md)
