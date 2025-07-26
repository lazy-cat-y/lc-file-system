[![Release](https://img.shields.io/badge/Release-v1.0.0-blueviolet?style=flat-square)](https://github.com/lazy-cat-y/lc-file-system/releases)
[![Last Commit](https://img.shields.io/badge/Last%20Commit-July%202025-brightgreen?style=flat-square)](https://github.com/lazy-cat-y/lc-file-system/commits)
[![License](https://img.shields.io/badge/License-MIT-red?style=flat-square)](https://opensource.org/licenses/MIT)
[![Stars](https://img.shields.io/github/stars/lazy-cat-y/lc-file-system?style=flat-square)](https://github.com/lazy-cat-y/lc-file-system/stargazers)
[![Issues](https://img.shields.io/github/issues/lazy-cat-y/lc-file-system?style=flat-square)](https://github.com/lazy-cat-y/lc-file-system/issues)
[![Code Size](https://img.shields.io/github/languages/code-size/lazy-cat-y/lc-file-system?style=flat-square)](https://github.com/lazy-cat-y/lc-file-system)
[![Contributors](https://img.shields.io/github/contributors/lazy-cat-y/lc-file-system?style=flat-square)](https://github.com/lazy-cat-y/lc-file-system/graphs/contributors)

# LC File System

## Overview

This project implements a custom file system in C++, designed with modularity and performance in mind. It includes features such as:
- Bitmap-based block and inode allocation
- Inode and directory management
- A block buffer pool with a clock-sweep eviction algorithm
- Multithreaded read/write support
- Support for testing via Google Test

The system simulates block-level storage and supports typical file system operations like file creation, reading, writing, and deletion. It is structured to be extensible and testable.

## Build Instructions

### Prerequisites

Make sure the following tools are installed:
- **CMake ≥ 3.20**
- **Clang ≥ 18** (GCC can also be used if compatible)
- **Make** or **Ninja** (depending on your generator)
- **GoogleTest** (automatically pulled in as a dependency)

You can check your compiler version with:
```bash
$ clang --version
Ubuntu clang version 18.1.3 (1ubuntu1)
Target: aarch64-unknown-linux-gnu
````

### Building the Project

Clone the repository and create a build directory:

```bash
git clone <your-repo-url>
cd <your-project-directory>
mkdir build && cd build
```

Run CMake to configure the project:

```bash
cmake .. -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
```

Then build the project:

```bash
cmake --build .
```

To run the tests:

```bash
./tests/<your_test_binary_name>
```

## Project Structure

* `src/` — Core implementation (block manager, inode manager, buffer pool, etc.)
* `include/` — Public headers
* `tests/` — Unit tests using GoogleTest
* `CMakeLists.txt` — Build configuration

---

## **License**
This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

---

## **Author**
- **Haoxin Yang**  
  GitHub: [lazy-cat-y](https://github.com/lazy-cat-y)