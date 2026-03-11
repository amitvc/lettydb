## LettyDB (WORK IN PROGRESS)
Welcome to the LettyDB SQL Database project! Once completely built it will be a fully functional, disk-based relational database management system (RDBMS) written from scratch in modern C++.
Designed as a comprehensive educational initiative, this project aims to demystify the "black box" of database internals. Rather than relying on existing libraries for core functionality, LettyDB implements the entire database stack from first principles. It serves as a practical exploration of system design, low-level memory management, and algorithm implementation. My goal is not to take any external dependency for the core implementation.

## Features

- Custom SQL Parser: A handwritten recursive descent parser that builds a detailed Abstract Syntax Tree (AST) from raw SQL queries.

- ACID Compliance: Designed to ensure Atomicity, Consistency, Isolation, and Durability for all transactions.

- Disk-Based Storage: A page-oriented storage engine that persists data to disk.

- B+ Tree Indexing: For efficient data retrieval and query optimization.

- Client-Server Architecture: A simple TCP-based server and client library for remote connections.

- Command-Line Interface (CLI): A user-friendly tool for interacting with the database.



## Prerequisites
- A C++17 compliant compiler (e.g., GCC, Clang, MSVC)

- CMake (version 3.14 or higher)

- Google Test (will be downloaded automatically by CMake)

- Doxygen (optional, for generating documentation)

## Building the Project
- Clone the repository:
```
git clone https://github.com/amitvc/lettydb.git
cd lettydb
```

- Configure with CMake:
```bash
mkdir -p cmake-build-debug
cd cmake-build-debug
cmake ..
```

- Compile the code:
```bash
cmake --build .
```

## Running Tests
To run the unit tests, navigate to the build directory and run `ctest`:
```bash
cd cmake-build-debug
ctest
```
