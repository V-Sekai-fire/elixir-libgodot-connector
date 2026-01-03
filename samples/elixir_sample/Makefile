# Builds the native NIF via CMake.
# This is invoked automatically by `mix compile` via elixir_make.

BUILD_DIR ?= build
CMAKE ?= cmake

# Default to a debuggable build for dev/test, and Release for prod.
CMAKE_BUILD_TYPE ?= Release
ifeq ($(MIX_ENV),dev)
	CMAKE_BUILD_TYPE := Debug
endif
ifeq ($(MIX_ENV),test)
	CMAKE_BUILD_TYPE := Debug
endif

.PHONY: all clean

all:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)
	$(CMAKE) --build $(BUILD_DIR) --target libgodot_nif

clean:
	rm -rf $(BUILD_DIR)
