cmake_minimum_required(VERSION 3.16)

project(libgodot_elixir_nif LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Locate Erlang/ERTS NIF headers.
execute_process(
  COMMAND erl -noshell -eval "io:format(\"~s\", [lists:concat([code:root_dir(), \"/erts-\", erlang:system_info(version), \"/include\"])])" -s init stop
  OUTPUT_VARIABLE ERL_INCLUDE_PATH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT ERL_INCLUDE_PATH)
  message(FATAL_ERROR "Could not determine ERL_INCLUDE_PATH; ensure 'erl' is on PATH")
endif()

set(GODOT_CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../godot-cpp)

message(STATUS "Building godot-cpp via add_subdirectory (incremental; first build may take a while)...")
# Keep the setup similar to the C++ sample (add_subdirectory), but isolate build artifacts
# in this project's binary tree so we don't reuse/overwrite ../../godot-cpp/build.
add_subdirectory(${GODOT_CPP_DIR} ${CMAKE_CURRENT_BINARY_DIR}/godot-cpp)

add_library(libgodot_nif MODULE src/libgodot_nif.cpp)

target_include_directories(libgodot_nif PRIVATE
  ${ERL_INCLUDE_PATH}
  ${GODOT_CPP_DIR}/include
  ${GODOT_CPP_DIR}/gen/include
  ${GODOT_CPP_DIR}/gdextension
)

target_link_libraries(libgodot_nif PRIVATE godot-cpp)

# When building via `mix compile` (elixir_make), write artefacts to the Mix priv dir.
# When building standalone, fall back to a local priv/ directory.
if(DEFINED ENV{MIX_APP_PATH})
  set(PRIV_DIR "$ENV{MIX_APP_PATH}/priv")
else()
  set(PRIV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/priv")
endif()

file(MAKE_DIRECTORY "${PRIV_DIR}")

# NIF is expected to be a .so on both macOS and Linux.
set_target_properties(libgodot_nif PROPERTIES
  OUTPUT_NAME "libgodot_nif"
  PREFIX ""
  SUFFIX ".so"
  LIBRARY_OUTPUT_DIRECTORY "${PRIV_DIR}"
)

# When distributing precompiled artefacts, we want libgodot to live next to the NIF in priv/.
if(APPLE)
  set(LIBGODOT_SOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../build/libgodot.dylib")
  set(LIBGODOT_DEST_PATH "${PRIV_DIR}/libgodot.dylib")
else()
  set(LIBGODOT_SOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../build/libgodot.so")
  set(LIBGODOT_DEST_PATH "${PRIV_DIR}/libgodot.so")
endif()

if(NOT EXISTS "${LIBGODOT_SOURCE_PATH}")
  message(FATAL_ERROR "Missing libgodot at '${LIBGODOT_SOURCE_PATH}'. Build it first (from repo root run ./build_libgodot.sh) or use precompiled artefacts via elixir_make.")
endif()

if(APPLE)
  add_custom_command(
    TARGET libgodot_nif POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBGODOT_SOURCE_PATH}" "${LIBGODOT_DEST_PATH}"
    # Normalize the install name so it doesn't refer to Godot's bin/ path.
    COMMAND install_name_tool -id "@rpath/libgodot.dylib" "${LIBGODOT_DEST_PATH}"
    VERBATIM
  )
else()
  add_custom_command(
    TARGET libgodot_nif POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBGODOT_SOURCE_PATH}" "${LIBGODOT_DEST_PATH}"
    VERBATIM
  )
endif()

if(APPLE)
  # Equivalent to: -undefined dynamic_lookup
  target_link_options(libgodot_nif PRIVATE "-undefined" "dynamic_lookup")
else()
  target_link_libraries(libgodot_nif PRIVATE dl)
endif()
