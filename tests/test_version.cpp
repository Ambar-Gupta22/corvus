#include <doctest/doctest.h>

#include <string>

#include "corvus/corvus.h"

// CORVUS_PROJECT_VERSION is injected by tests/CMakeLists.txt from
// project(VERSION ...), so a version bump in one place but not the other fails.
TEST_CASE("CORVUS_VERSION_* macros match the CMake project version") {
    const std::string header = std::to_string(CORVUS_VERSION_MAJOR) + "." +
                               std::to_string(CORVUS_VERSION_MINOR) + "." +
                               std::to_string(CORVUS_VERSION_PATCH);
    CHECK(header == CORVUS_PROJECT_VERSION);
}
