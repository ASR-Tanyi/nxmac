// JSON as Python's json module writes it (src/dmsh/json.h): the expected texts are Python 3.13's.
#include <string>
#include <utility>

#include "gtest/gtest.h"
#include "json.h"

namespace nxmac::dmsh {
namespace {

TEST(JsonTest, Indent1AsJsonDump) {
    Json d = Json::MakeObject();
    d["e"] = 1e-07;
    d["f"] = 1e16;
    d["g"] = 123456789.125;
    d["h"] = 0.1;
    d["i"] = 5e-324;
    d["j"] = 1.0;
    d["k"] = -0.0;
    Json v = Json::MakeObject();
    v["a"] = Json::Array{1, 2.5, "x\n\"\xc3\xa9", nullptr, true, Json::MakeObject()};
    v["b"] = Json::MakeObject();
    v["c"] = Json::MakeArray();
    v["d"] = d;
    EXPECT_EQ(JsonIndent1(v),
              "{\n \"a\": [\n  1,\n  2.5,\n  \"x\\n\\\"\xc3\xa9\",\n  null,\n  true,\n  {}\n ],\n \"b\": {},\n \"c\": [],\n \"d\": {\n"
              "  \"e\": 1e-07,\n  \"f\": 1e+16,\n  \"g\": 123456789.125,\n  \"h\": 0.1,\n  \"i\": 5e-324,\n  \"j\": 1.0,\n  \"k\": -0.0\n }\n}");
}

TEST(JsonTest, OneLineAndSorted) {
    Json v = Json::MakeObject();
    v["a"] = Json::Array{1, "\xc3\xa9\xf0\x9f\x98\x80"};
    v["b"] = 2.0;
    EXPECT_EQ(JsonLine(v), "{\"a\": [1, \"\\u00e9\\ud83d\\ude00\"], \"b\": 2.0}");
    Json w = Json::MakeObject();
    w["b"] = 1;
    w["a"] = Json::Object{{"d", Json::Array{1, 2}}, {"c", nullptr}};
    EXPECT_EQ(JsonSortedCompact(w), R"({"a":{"c":null,"d":[1,2]},"b":1})");
}

TEST(JsonTest, FloatRepr) {
    EXPECT_EQ(PyFloatRepr(1e22), "1e+22");
    EXPECT_EQ(PyFloatRepr(1.5e-5), "1.5e-05");
    EXPECT_EQ(PyFloatRepr(123456789012345680.0), "1.2345678901234568e+17");
    EXPECT_EQ(PyFloatRepr(0.0001), "0.0001");
    EXPECT_EQ(PyFloatRepr(9999999999999998.0), "9999999999999998.0");
    EXPECT_EQ(PyFloatRepr(9007199254740992.0), "9007199254740992.0");
}

TEST(JsonTest, ParsesAndKeepsOrder) {
    const auto v = JsonParse("{\"b\": [1, -2, 3.5, \"a\\u00e9\\n\", true, false, null], \"a\": {\"x\": {}}, \"b2\": 1e3} ");
    ASSERT_TRUE(v.ok()) << v.status();
    EXPECT_EQ(v->object()[0].first, "b");
    EXPECT_EQ(v->at("b").array()[1].as_int(), -2);
    EXPECT_EQ(v->at("b").array()[3].as_string(), "a\xc3\xa9\n");
    EXPECT_TRUE(v->at("b2").kind() == Json::Kind::kFloat);
    EXPECT_EQ(JsonLine(*v), "{\"b\": [1, -2, 3.5, \"a\\u00e9\\n\", true, false, null], \"a\": {\"x\": {}}, \"b2\": 1000.0}");
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "nul", "1 2", "\"abc"}) EXPECT_FALSE(JsonParse(bad).ok()) << bad;
}

TEST(JsonTest, ParsesFloatsAsPython) {
    // repr(json.loads(text)), Python 3.13, including past the range of a double
    const std::pair<const char*, const char*> cases[] = {
        {"1e400", "inf"},
        {"-1e400", "-inf"},
        {"1e-400", "0.0"},
        {"-1e-400", "-0.0"},
        {"4.9e-324", "5e-324"},
        {"2.5e-324", "5e-324"},
        {"2.4e-324", "0.0"},
        {"1.7976931348623157e308", "1.7976931348623157e+308"},
        {"1.7976931348623159e308", "inf"},
        {"2.2250738585072011e-308", "2.225073858507201e-308"},
        {"0.1", "0.1"},
    };
    for (const auto& [text, repr] : cases) {
        const auto v = JsonParse(text);
        ASSERT_TRUE(v.ok()) << text << ": " << v.status();
        EXPECT_EQ(PyFloatRepr(v->as_float()), repr) << text;
    }
}

TEST(JsonTest, DictOperations) {
    Json d = Json::MakeObject();
    d["x"] = 1;
    d["y"] = 2;
    d["x"] = 3;  // an old key keeps its place
    EXPECT_EQ(JsonLine(d), R"({"x": 3, "y": 2})");
    Json e = Json::Object{{"y", 5}, {"z", 6}};
    d.update(e);
    EXPECT_EQ(JsonLine(d), R"({"x": 3, "y": 5, "z": 6})");
    EXPECT_TRUE(d.erase("x"));
    EXPECT_FALSE(d.erase("x"));
    EXPECT_FALSE(d.contains("x"));
    const Json ab = Json::Object{{"a", 1}, {"b", 2}};
    const Json ba = Json::Object{{"b", 2}, {"a", 1}};
    EXPECT_TRUE(ab == ba);  // dicts compare without order
    const Json l12 = Json::Array{1, 2};
    const Json l21 = Json::Array{2, 1};
    EXPECT_FALSE(l12 == l21);
    EXPECT_FALSE(Json().truthy());
    EXPECT_FALSE(Json(Json::MakeArray()).truthy());
    EXPECT_TRUE(Json("x").truthy());
}

}  // namespace
}  // namespace nxmac::dmsh
