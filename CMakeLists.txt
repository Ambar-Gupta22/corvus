# 3.18: FetchContent_Declare(SOURCE_SUBDIR) is honored from 3.18 on — below it
# MakeAvailable would add_subdirectory the fetched deps and pollute our
# install/export surface.
cmake_minimum_required(VERSION 3.18)
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
option(CORVUS_ENABLE_TLS     "HTTPS in the default HTTP transport (needs OpenSSL)" ON)

# Default to a Release build if the user didn't pick one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Third-party (header-only, fetched like doctest: sources only, no
# add_subdirectory — keeps our install/export surface clean) ------------------
include(FetchContent)
FetchContent_Declare(
  cpp_httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.18.3
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  do-not-configure
)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  do-not-configure
)
FetchContent_MakeAvailable(cpp_httplib nlohmann_json)

# --- Library ---------------------------------------------------------------
add_library(corvus
  src/tool_registry.cpp
  src/memory.cpp
  src/schema.cpp
  src/mock_llm.cpp
  src/agent.cpp
  src/agent_builder.cpp
  src/clients_stub.cpp
  src/httplib_transport.cpp
)
add_library(corvus::corvus ALIAS corvus)

target_include_directories(corvus PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(corvus PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(corvus PUBLIC Threads::Threads)

# Third-party headers are PRIVATE: nothing in include/corvus/ exposes them.
# cpp-httplib is compiled in exactly one TU (src/httplib_transport.cpp).
target_include_directories(corvus PRIVATE
  ${cpp_httplib_SOURCE_DIR}
  ${nlohmann_json_SOURCE_DIR}/single_include
)

set(CORVUS_TLS_ENABLED OFF)
if(CORVUS_ENABLE_TLS)
  find_package(OpenSSL)
  if(OPENSSL_FOUND)
    set(CORVUS_TLS_ENABLED ON)
    target_compile_definitions(corvus PRIVATE CORVUS_HAS_TLS)
    target_link_libraries(corvus PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  else()
    message(WARNING "OpenSSL not found — corvus builds without HTTPS; "
                    "https URLs will fail at runtime with a clear error. "
                    "Install OpenSSL or set -DCORVUS_ENABLE_TLS=OFF to silence this.")
  endif()
endif()

if(WIN32)
  # cpp-httplib needs winsock; crypt32 loads system CA certs under OpenSSL.
  target_link_libraries(corvus PRIVATE ws2_32)
  if(CORVUS_TLS_ENABLED)
    target_link_libraries(corvus PRIVATE crypt32)
  endif()
endif()

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
