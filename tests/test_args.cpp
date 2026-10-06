#include <doctest/doctest.h>

#include <string>

#include "corvus/args.h"

using namespace corvus;

TEST_CASE("Args parses a JSON object and reads typed values") {
    auto a = Args::parse(R"({"s":"hi","n":2.5,"i":7,"b":true})");
    REQUIRE(a.has_value());
    CHECK(a->getString("s") == "hi");
    CHECK(a->getNumber("n") == 2.5);
    CHECK(a->getInteger("i") == 7);
    CHECK(a->getBool("b") == true);
}

TEST_CASE("Args treats empty text as an empty object") {
    auto a = Args::parse("");
    REQUIRE(a.has_value());
    CHECK_FALSE(a->has("x"));
    CHECK(Args::parse("  \n ").has_value());
}

TEST_CASE("Args rejects invalid JSON and non-object top levels with a reason") {
    std::string err;
    CHECK_FALSE(Args::parse("{not json", &err).has_value());
    CHECK(err == "arguments are not valid JSON");

    err.clear();
    CHECK_FALSE(Args::parse("[1,2]", &err).has_value());
    CHECK(err == "arguments must be a JSON object");
    CHECK_FALSE(Args::parse("42").has_value());  // null error pointer is fine
}

TEST_CASE("Args getters are strict about JSON types") {
    auto a = Args::parse(R"({"s":"5","n":5,"b":1})");
    REQUIRE(a.has_value());
    CHECK_FALSE(a->getNumber("s").has_value());   // no string->number coercion
    CHECK_FALSE(a->getInteger("s").has_value());
    CHECK_FALSE(a->getString("n").has_value());
    CHECK_FALSE(a->getBool("b").has_value());     // 1 is not a boolean
    CHECK(a->getNumber("n") == 5.0);              // number accepts an integer
    CHECK_FALSE(a->getString("missing").has_value());
}

TEST_CASE("Args treats JSON null as absent") {
    auto a = Args::parse(R"({"x":null})");
    REQUIRE(a.has_value());
    CHECK_FALSE(a->has("x"));
    CHECK_FALSE(a->getString("x").has_value());
}

TEST_CASE("Args integer accepts exactly integral floats and enforces int64 range") {
    auto a = Args::parse(
        R"({"whole":3.0,"frac":3.5,"neg":-4,"max":9223372036854775807,)"
        R"("over":9223372036854775808,"huge":1e300})");
    REQUIRE(a.has_value());
    CHECK(a->getInteger("whole") == 3);
    CHECK_FALSE(a->getInteger("frac").has_value());
    CHECK(a->getInteger("neg") == -4);
    CHECK(a->getInteger("max") == 9223372036854775807LL);
    CHECK_FALSE(a->getInteger("over").has_value());
    CHECK_FALSE(a->getInteger("huge").has_value());
}
