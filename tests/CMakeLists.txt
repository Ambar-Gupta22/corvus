include(FetchContent)

# doctest is header-only. We fetch it but deliberately skip its own CMake:
# SOURCE_SUBDIR points at a non-existent dir so MakeAvailable downloads the
# sources without add_subdirectory'ing them. (doctest's bundled CMake declares
# cmake_minimum_required < 3.5, which CMake 4+ refuses to process.)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
  SOURCE_SUBDIR  do-not-configure
)
FetchContent_MakeAvailable(doctest)

add_library(doctest_header INTERFACE)
target_include_directories(doctest_header INTERFACE ${doctest_SOURCE_DIR})

add_executable(corvus_tests
  test_main.cpp
  test_schema.cpp
  test_registry.cpp
  test_agent.cpp
)
target_link_libraries(corvus_tests PRIVATE corvus::corvus doctest_header)

add_test(NAME corvus_tests COMMAND corvus_tests)
