include(FetchContent)

FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
)
FetchContent_MakeAvailable(doctest)

add_executable(corvus_tests
  test_main.cpp
  test_schema.cpp
  test_registry.cpp
  test_agent.cpp
)
target_link_libraries(corvus_tests PRIVATE corvus::corvus doctest::doctest)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(corvus_tests)
