# Simple Makefile for building the port executable

.PHONY: all clean

# Detect number of CPU cores for parallel builds
JOBS := $(shell sysctl -n hw.logicalcpu 2>/dev/null || nproc)

all: build_port

build_port:
	@echo "Building libgodot_port executable..."
	@mkdir -p build
	@cd build && cmake -S .. -B . -DCMAKE_BUILD_TYPE=Release
	@cmake --build build --target libgodot_port -j$(JOBS)
	@echo "Port executable built in build/priv/libgodot_port"

clean:
	@echo "Cleaning build artifacts..."
	@rm -rf build
	@rm -rf _build
	@rm -rf priv

help:
	@echo "Available targets:"
	@echo "  all        - Build the port executable (default)"
	@echo "  build_port - Build the port executable"
	@echo "  clean      - Remove build artifacts"
	@echo "  help       - Show this help message"