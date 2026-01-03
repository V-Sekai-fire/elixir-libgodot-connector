cmake_minimum_required(VERSION 3.16)

project(libgodot_port CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Port executable
add_executable(libgodot_port src/libgodot_port.cpp)

target_include_directories(libgodot_port PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

if(APPLE)
  target_link_options(libgodot_port PRIVATE "-undefined" "dynamic_lookup")
else()
  target_link_libraries(libgodot_port PRIVATE dl)
endif()

# Copy to priv directory
if(DEFINED ENV{MIX_APP_PATH})
  set(PRIV_DIR "$ENV{MIX_APP_PATH}/priv")
else()
  set(PRIV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/priv")
endif()

file(MAKE_DIRECTORY "${PRIV_DIR}")

set_target_properties(libgodot_port PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PRIV_DIR}"
)