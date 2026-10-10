#!/bin/bash

# Gomoku AI Build Script for WSL/Linux

set -e

echo "=== Gomoku AI Build Script ==="
echo ""

# Check for required tools
echo "Checking build tools..."

if ! command -v g++ &> /dev/null; then
    echo "Error: g++ not found. Please install build-essential:"
    echo "  sudo apt-get install build-essential"
    exit 1
fi

if ! command -v cmake &> /dev/null; then
    echo "Warning: cmake not found. Using direct compilation."
    USE_CMAKE=0
else
    USE_CMAKE=1
fi

# Check for Boost
if [ ! -d "/usr/include/boost" ]; then
    echo "Error: Boost not found. Please install libboost-all-dev:"
    echo "  sudo apt-get install libboost-all-dev"
    exit 1
fi

echo "Build tools OK"
echo ""

# Create build directory
mkdir -p build
cd build

if [ "$USE_CMAKE" -eq 1 ]; then
    echo "Building with CMake..."
    cmake ..
    make -j$(nproc)
else
    echo "Building with direct compilation..."
    
    # Download dependencies if not present
    if [ ! -d "../deps/websocketpp" ]; then
        echo "Downloading websocketpp..."
        mkdir -p ../deps
        cd ../deps
        git clone --depth 1 --branch 0.8.2 https://github.com/zaphoyd/websocketpp.git
        cd ..
        cd build
    fi
    
    if [ ! -d "../deps/json" ]; then
        echo "Downloading nlohmann/json..."
        mkdir -p ../deps
        cd ../deps
        git clone --depth 1 --branch v3.11.2 https://github.com/nlohmann/json.git
        cd ..
        cd build
    fi
    
    # Compile
    echo "Compiling..."
    g++ -std=c++17 -O2 -Wall -Wextra \
        -I../src \
        -I../deps/websocketpp \
        -I../deps/json/include \
        -I/usr/include/boost \
        ../src/main.cpp \
        ../src/core/board.cpp \
        ../src/core/pattern.cpp \
        ../src/core/evaluator.cpp \
        ../src/search/greedy.cpp \
        ../src/search/vcf.cpp \
        ../src/search/vct.cpp \
        ../src/search/alphabeta.cpp \
        ../src/search/search_engine.cpp \
        ../src/search/controller.cpp \
        ../src/optimization/transposition.cpp \
        ../src/optimization/opening_book.cpp \
        ../src/server/server.cpp \
        ../src/server/game_manager.cpp \
        -o gomoku_server \
        -lboost_system -lboost_thread -lpthread
    
    # Copy web files
    echo "Copying web files..."
    cp -r ../web .
    cp -r ../data .
    
    echo ""
    echo "Build complete!"
fi

echo ""
echo "=== Build Successful ==="
echo ""
echo "To run the server:"
echo "  cd build"
echo "  ./gomoku_server"
echo ""
echo "Then open http://localhost:8080 in your browser"
echo ""
