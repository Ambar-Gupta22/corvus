cmake_minimum_required(VERSION 3.16)
project(corvus VERSION 0.0.1 LANGUAGES CXX)

# --- Options ---------------------------------------------------------------
# Tests default ON only when corvus is the top-level project, so consumers
# pulling corvus via FetchContent/add_subdirectory don't inherit our tests.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(CORVUS_IS_TOP_LEVEL ON)
else()
  set(CORVUS_IS_TOP_LEVEL OFF)
endif()

option(CORVUS_BUILD_TESTS    "Build the test suite"      ${CORVUS_IS_TOP_LEVEL})
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

# --- Install / export -------------------------------------------------------
# Makes both consumption modes work: FetchContent/add_subdirectory (via the
# corvus::corvus alias above) and an installed find_package(corvus).
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS corvus
  EXPORT corvusTargets
  ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT corvusTargets
  NAMESPACE corvus::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/corvus
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/corvusConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/corvusConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/corvus
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/corvusConfigVersion.cmake
  COMPATIBILITY SameMajorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/corvusConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/corvusConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/corvus
)

# --- Tests / examples ------------------------------------------------------
if(CORVUS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CORVUS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
