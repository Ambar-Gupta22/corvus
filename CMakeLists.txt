cmake_minimum_required(VERSION 3.16)
project(corvus VERSION 0.0.1 LANGUAGES CXX)

# --- Options ---------------------------------------------------------------
option(CORVUS_BUILD_TESTS    "Build the test suite"      ON)
option(CORVUS_BUILD_EXAMPLES "Build the examples"        OFF)
option(CORVUS_WITH_LLAMACPP  "Enable llama.cpp backend"  OFF)  # Phase 2

# Default to a Release build if the user didn't pick one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Library ---------------------------------------------------------------
add_library(corvus
  src/tool_registry.cpp
  src/memory.cpp
  src/schema.cpp
  src/mock_llm.cpp
  src/agent.cpp
  src/agent_builder.cpp
  src/clients_stub.cpp
)
add_library(corvus::corvus ALIAS corvus)

target_include_directories(corvus PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(corvus PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(corvus PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(corvus PRIVATE /W4)
else()
  target_compile_options(corvus PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- Tests / examples ------------------------------------------------------
if(CORVUS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CORVUS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
