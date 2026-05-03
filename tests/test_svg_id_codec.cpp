// test_svg_id_codec.cpp ───────────────────────────────────────────────────
//
// Standalone test for curvz::utils SVG-id encoder.
//
// Build:
//   g++ -std=c++17 -Iinclude -o tests/test_svg_id_codec
//       tests/test_svg_id_codec.cpp src/curvz_utils.cpp
// Run:
//   ./tests/test_svg_id_codec
//
// Expected: "All N tests passed." Otherwise prints failing case and exits 1.
//
// The codec is one-way: Curvz writes SVG ids, never reads them back as
// authoritative. Source of truth on load is data-curvz-name and
// data-curvz-iid attributes — not the SVG id. Tests cover sanitise,
// short_iid, and the composed encoder. No decode tests because there
// is no decode.

#include "curvz_utils.hpp"

#include <iostream>
#include <string>
#include <vector>

using curvz::utils::sanitise_for_xml_id;
using curvz::utils::short_iid;
using curvz::utils::encode_svg_id;

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_EQ(a, b, label) do {                                     \
    auto _a = (a);                                                      \
    auto _b = (b);                                                      \
    if (_a == _b) { ++g_pass; }                                         \
    else {                                                              \
        ++g_fail;                                                       \
        std::cerr << "FAIL: " << label                                  \
                  << "\n  got:      [" << _a << "]"                     \
                  << "\n  expected: [" << _b << "]"                     \
                  << "\n  at " << __FILE__ << ":" << __LINE__ << "\n";  \
    }                                                                   \
} while (0)

static const char* kIidA = "12345678-1234-1234-1234-1234567890ab";
static const char* kIidB = "abcdef01-2345-6789-abcd-ef0123456789";
static const std::string kPilcrow = "\xC2\xB6";

int main() {
    // ── sanitise_for_xml_id ─────────────────────────────────────────
    EXPECT_EQ(sanitise_for_xml_id("Layer 1"),       "Layer_1",
              "single space → underscore");
    EXPECT_EQ(sanitise_for_xml_id("Layer  1"),      "Layer_1",
              "double space collapses");
    EXPECT_EQ(sanitise_for_xml_id("a/b<c>d&e"),     "abcde",
              "specials dropped");
    EXPECT_EQ(sanitise_for_xml_id(""),              "_",
              "empty becomes underscore");
    EXPECT_EQ(sanitise_for_xml_id("123abc"),        "_123abc",
              "leading digit gets underscore");
    EXPECT_EQ(sanitise_for_xml_id("-foo"),          "_-foo",
              "leading hyphen gets underscore");
    EXPECT_EQ(sanitise_for_xml_id(".foo"),          "_.foo",
              "leading dot gets underscore");
    EXPECT_EQ(sanitise_for_xml_id("Layer_1"),       "Layer_1",
              "underscore preserved");
    EXPECT_EQ(sanitise_for_xml_id("Star 3 (Copy)"), "Star_3_Copy",
              "parens dropped, spaces collapsed");
    // Pilcrow key-trap: literal pilcrow in input must be dropped.
    EXPECT_EQ(sanitise_for_xml_id("Hello" + kPilcrow + "World"),
              "HelloWorld",
              "pilcrow stripped from input");

    // ── short_iid ───────────────────────────────────────────────────
    EXPECT_EQ(short_iid(kIidA),     "12345678",
              "short_iid: first 8 chars");
    EXPECT_EQ(short_iid(kIidB),     "abcdef01",
              "short_iid: alt uuid");
    EXPECT_EQ(short_iid("short"),   "short",
              "short_iid: input shorter than 8 returns input");
    EXPECT_EQ(short_iid(""),        "",
              "short_iid: empty → empty");

    // ── encode_svg_id ───────────────────────────────────────────────
    EXPECT_EQ(encode_svg_id("Layer 1", kIidA),
              std::string("Layer_1") + kPilcrow + "12345678",
              "encode: name + iid → name¶short_iid");
    EXPECT_EQ(encode_svg_id("",        kIidA),
              std::string("12345678"),
              "encode: empty name → short_iid only");
    EXPECT_EQ(encode_svg_id("Layer 1", ""),
              std::string("Layer_1"),
              "encode: empty iid → sanitised name only");
    EXPECT_EQ(encode_svg_id("",        ""),
              std::string(""),
              "encode: both empty → empty");
    EXPECT_EQ(encode_svg_id("My Path (copy)", kIidB),
              std::string("My_Path_copy") + kPilcrow + "abcdef01",
              "encode: complex name");
    // Sanitise idempotence at encode level: clean and dirty inputs
    // produce the same output.
    EXPECT_EQ(encode_svg_id("Layer 1", kIidA),
              encode_svg_id("Layer_1", kIidA),
              "encode: space and underscore name produce same id");

    std::cout << g_pass << " passed, " << g_fail << " failed.\n";
    return g_fail == 0 ? 0 : 1;
}
