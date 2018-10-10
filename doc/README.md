LockTrip
========

Setup
---------------------
LockTrip is the original LockTrip Blockchain client and it builds the backbone of the network. It downloads and, by default, stores the entire history of LockTrip transactions; depending on the speed of your computer and network connection, the synchronization process can take anywhere from a few hours to a day or more.

To download LockTrip, visit [the download page](https://github.com/LockTrip/Blockchain/releases).

Running
---------------------
The following are some helpful notes on how to run LockTrip on your native platform.

### Unix

Unpack the files into a directory and run:

- `bin/locktrip-qt` (GUI) or
- `bin/locktripd` (headless)

### Windows

Unpack the files into a directory, and then run locktrip-qt.exe.

### OS X

Drag LockTrip to your applications folder, and then run LockTrip.

### Need Help?

<!--* See the documentation at the [Bitcoin Wiki](https://en.bitcoin.it/wiki/Main_Page)-->
<!--for help and more information.-->
* Ask for help on the [issues page](https://github.com/LockTrip/Blockchain/issues).

Building
---------------------
The following are developer notes on how to build LockTrip on your native platform. They are not complete guides, but include notes on the necessary libraries, compile flags, etc.

- [OS X Build Notes](build-osx.md)
- [Unix Build Notes](build-unix.md)
- [Windows Build Notes](build-windows.md)
- [Gitian Building Guide](gitian-building.md)

Development
---------------------
The LockTrip repo's [root README](/README.md) contains relevant information on the development process and automated testing.

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
