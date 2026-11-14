
# ExchangeHub

ExchangeHub is a multi-client file sharing and group communication application built using C++ and Linux TCP sockets.

## Features

- Multiple simultaneous client connections
- TCP-based file sharing
- Chunk-based file transmission
- Resume interrupted uploads
- Resume interrupted downloads
- SHA-256 file integrity verification
- Concurrent server-side client handling
- Terminal-based client interface
- Real-time group chat
- Transfer history and shared file library

## Technology Stack

- C++
- Linux
- TCP/IP sockets
- POSIX sockets and C++ threads
- OpenSSL SHA-256
- CMake

## Project Structure

```text
exchange-hub/
├── include/
│   └── common.hpp
├── src/
│   ├── server.cpp
│   └── client.cpp
├── CMakeLists.txt
├── README.md
└── .gitignore
```

## Build

```bash
sudo apt install build-essential cmake libssl-dev
cmake -S . -B build
cmake --build build -j
```

## Run the Server

```bash
./build/exchangehub_server
```

## Run a Client

```bash
./build/exchangehub_client
```

To connect to a server on another machine:

```bash
./build/exchangehub_client SERVER_IP
```

The server stores uploaded files in its `storage` directory.

## Resume Support

Incomplete uploads are retained as `.part` files on the server.

Incomplete downloads are retained as `.part` files on the client.

When a transfer is retried, it continues from the existing partial file size.

## Integrity Verification

SHA-256 hashes are checked after file transfer completion.

## Learning Goals

This project explores:

- TCP socket programming
- Client-server architecture
- Linux system programming
- Concurrent connections
- File I/O
- Reliable data transfer
- Group communication
- Error handling
- Cryptographic hashing
