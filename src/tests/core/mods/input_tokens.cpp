// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>
#include <catch2/catch_test_macros.hpp>
#include "core/mods/mod_input_tokens.h"

using namespace Core::Mods::InputTokens;

TEST_CASE("Stick aliases cover both sticks, orders and all quadrants", "[dsmod][input]") {
    for (const char stick : {'L', 'R'}) {
        for (const char x : {'+', '-'}) {
            for (const char y : {'+', '-'}) {
                const std::string a = std::string{stick} + 'X' + x;
                const std::string b = std::string{stick} + 'Y' + y;
                for (const auto& token : {a + b, b + a}) {
                    const auto position = ParseStick(token);
                    REQUIRE(position);
                    CHECK(position->right == (stick == 'R'));
                    CHECK(std::abs(position->x - (x == '+' ? 0.70710678f : -0.70710678f)) < 1e-6);
                    CHECK(std::abs(position->y - (y == '+' ? 0.70710678f : -0.70710678f)) < 1e-6);
                }
            }
        }
    }
    CHECK(ParseStick("LX+")->x == 1.0f);
    CHECK(ParseStick("RY-")->y == -1.0f);
}

TEST_CASE("Analog vectors preserve strength and safely clamp magnitude", "[dsmod][input]") {
    const auto partial = ParseStick(" LStick( +0.3, -0.4 ) ");
    REQUIRE(partial);
    CHECK(std::abs(partial->x - 0.3f) < 1e-6);
    CHECK(std::abs(partial->y + 0.4f) < 1e-6);
    CHECK(ParseStick("RStick(0,0)")->right);
    CHECK(ParseStick("RStick(0,0)")->x == 0.0f);
    const auto large = ParseStick("RStick(3,4)");
    REQUIRE(large);
    CHECK(std::abs(large->x - 0.6f) < 1e-6);
    CHECK(std::abs(large->y - 0.8f) < 1e-6);
    const auto huge = ParseStick("LStick(1e308,-1e308)");
    REQUIRE(huge);
    CHECK(std::abs(huge->x - 0.70710678f) < 1e-6);
    CHECK(std::abs(huge->y + 0.70710678f) < 1e-6);
}

TEST_CASE("Malformed and nonfinite stick vectors are rejected", "[dsmod][input]") {
    for (const auto text :
         {"LX+RX-", "LX+LX-", "LStick(nan,0)", "RStick(0,inf)", "LStick(1e999,0)", "LStick(1,2,3)",
          "LStick(,0)", "LStick(+-1,0)", "LStick(1x,0)", "LStick(1,0)junk", "LXjunk+"}) {
        INFO(text);
        CHECK_FALSE(ParseStick(text));
    }
}

TEST_CASE("Chords handle signed axes, vectors and buttons without ambiguity", "[dsmod][input]") {
    const auto valid = [](std::string_view text) {
        return text == "A" || text == "B" || text == "L" || text == "R";
    };
    for (const auto text :
         {"L+R", "LX++A", "A+RY-", "LX+LY-+B", "LStick(+0.2,-0.5)+RStick(1,0)+A"}) {
        INFO(text);
        REQUIRE(ParseChord(text, valid));
    }
    const auto chord = ParseChord("A+LStick(0.3,-0.4)+RStick(0,1)", valid);
    REQUIRE(chord);
    CHECK(chord->size() == 3);
    for (const auto text : {"A+bad", "A+", "+A", "A++B", "LX+RX-", "LX++LY-", "LStick(0,1)+LX+",
                            "A+LStick(1,(2))", "A+LStick(1,0"}) {
        INFO(text);
        CHECK_FALSE(ParseChord(text, valid));
    }
}
