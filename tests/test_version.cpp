#include <doctest/doctest.h>

#include <parameda/version.hpp>

TEST_CASE("version string is non-empty") {
    CHECK(!parameda::VERSION.empty());
    CHECK(parameda::version() == parameda::VERSION);
}
