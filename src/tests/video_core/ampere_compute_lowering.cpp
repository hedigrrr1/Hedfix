// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <utility>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

TEST_CASE("Ampere LEA.HI with a constant base and no carry lowers", "[video_core][ampere]") {
    // LEA.HI R1, R17, c[0][0x0], RZ, 0x1e from Pokemon Pokopia's compute shaders.
    const std::array<Instruction, 2> code{{{{0x11017a11, 0x00c00000, 0x078ff0ff, 0x000fe400}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const auto& out = lowered.instructions;
    // R250 = R17 >> 2 (32 - 0x1e, unsigned), then R1 = R250 + the constant.
    REQUIRE(std::find(out.begin(), out.end(), 0x38280000002711faULL) != out.end());
    REQUIRE(std::find(out.begin(), out.end(), 0x5c1000000fb7fa01ULL) != out.end());
}

TEST_CASE("Ampere LEA.HI with a constant base still rejects a carry out", "[video_core][ampere]") {
    // The same LEA.HI writing a carry out to P0 has no lowering yet.
    Instruction carry{{0x11017a11, 0x00c00000, 0x078ff0ff, 0x000fe400}};
    carry.words[2] &= ~(7U << 17);
    const std::array<Instruction, 2> code{{carry, Exit}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
}

TEST_CASE("Ampere LEA.HI with a negated register base lowers to IADD with a negated B",
          "[video_core][ampere][oot]") {
    // LEA.HI R1, R1, -R0, RZ, 0x1e from Ocarina's per-tap gather compute program.
    const Instruction lea{{0x01017211, 0x80000000, 0x078ff0ff, 0x000fca00}};
    const auto lowered = LowerToMaxwell(std::array{lea, Exit});
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const auto& out = lowered.instructions;
    // R250 = R1 >> 2 (32 - 0x1e, unsigned), then R1 = R250 - R0.
    REQUIRE(std::find(out.begin(), out.end(), 0x38280000002701faULL) != out.end());
    REQUIRE(std::find(out.begin(), out.end(), 0x5c1100000007fa01ULL) != out.end());
    // A negated base with a carry in (.X) has no lowering.
    auto extended = lea;
    extended.words[2] |= 1U << 10;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{extended, Exit})));
}

TEST_CASE("Ampere indexed ULDC of bytes and halfwords lowers to an indexed Maxwell LDC",
          "[video_core][ampere][oot]") {
    // ULDC.U8 / ULDC.U16 UR, c[bank][UR + offset] from Ocarina's compute programs.
    const std::array<std::pair<Instruction, u64>, 4> captured{{
        {{{0x06087abb, 0x01002080, 0x08000000, 0x000fe400}}, 0xef9000400827dee0ULL},
        {{{0x06077abb, 0x01002080, 0x08000000, 0x000fe400}}, 0xef9000400827dedfULL},
        {{{0x0d047abb, 0x014000c0, 0x08000000, 0x000fe400}}, 0xef9000500037e5dcULL},
        {{{0x11047abb, 0x01400000, 0x08000400, 0x000fe400}}, 0xef9200500007e9dcULL},
    }};
    for (const auto& [uldc, ldc] : captured) {
        const auto lowered = LowerToMaxwell(std::array{uldc, Exit});
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        const auto& out = lowered.instructions;
        CHECK(std::find(out.begin(), out.end(), ldc) != out.end());
    }
    // A halfword at an odd offset has no LDC form.
    auto odd = captured[3].first;
    odd.words[1] |= 1U << 6;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{odd, Exit})));
}

TEST_CASE("Ampere ULEA carries through UP2 and UP3 into UIADD3.X", "[video_core][ampere][oot]") {
    // ULEA UR4, UR4, UR11, 0x2 with its carry in UP2 (UP3), then UIADD3.X UR5, URZ, UR5, URZ,
    // UP2 (UP3): the 64-bit address arithmetic of Ocarina's compute programs.
    const std::array<std::pair<std::array<Instruction, 3>, u32>, 2> sequences{{
        {{{{{0x0b047291, 0x00000004, 0x0f84103f, 0x000fc600}},
           {{0x3f057290, 0x00000005, 0x097fe43f, 0x000fe400}}, Exit}}, 2},
        {{{{{0x070a7291, 0x0000000a, 0x0f86103f, 0x000fe200}},
           {{0x3f0b7290, 0x0000000b, 0x09ffe43f, 0x000fe200}}, Exit}}, 3},
    }};
    for (const auto& [code, up] : sequences) {
        const auto lowered = LowerToMaxwell(code);
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        const auto& out = lowered.instructions;
        const u64 slot = 6 - up;
        // ISETP writes the carry to P(6-n); IADD3.X selects it back from P(6-n).
        CHECK(std::any_of(out.begin(), out.end(), [&](u64 i) {
            return ((i >> 48) & ~3ULL) == 0x5b60 && ((i >> 3) & 7) == slot;
        }));
        CHECK(std::any_of(out.begin(), out.end(), [&](u64 i) {
            return (i >> 48) == 0x5ca0 && ((i >> 39) & 7) == slot;
        }));
    }
}

TEST_CASE("Ampere UIADD3 with an immediate carries through UP2 into UIADD3.X",
          "[video_core][ampere][oot]") {
    // UIADD3 UR10, UP2, UR10, 0xc, URZ then UIADD3.X UR11, URZ, UR11, URZ, UP2 from Ocarina.
    const std::array<Instruction, 3> code{{{{0x0a0a7890, 0x0000000c, 0x0ff5e03f, 0x000fe200}},
                                           {{0x3f0b7290, 0x0000000b, 0x097fe43f, 0x000fe200}},
                                           Exit}};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const auto& out = lowered.instructions;
    CHECK(std::any_of(out.begin(), out.end(), [](u64 i) {
        return (i >> 48) == 0x5ca0 && ((i >> 39) & 7) == 4;
    }));
}

TEST_CASE("Ampere F2IP.U8.F32 packs two clamped bytes", "[video_core][ampere][oot]") {
    // F2IP.U8.F32 R41, R40, R39, RZ and R42, R42, R37, RZ from Ocarina's RGBA8 packing.
    for (const auto& [insn, expected] :
         {std::pair{Instruction{{0x28297243, 0x00000027, 0x000004ff, 0x000fc400}},
                    0x5c1804000f87f929ULL},
          std::pair{Instruction{{0x2a2a7243, 0x00000025, 0x000004ff, 0x000fe400}},
                    0x5c1804000f87f92aULL}}) {
        const std::array<Instruction, 2> code{{insn, Exit}};
        const auto lowered = LowerToMaxwell(code);
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        const auto& out = lowered.instructions;
        // Two F2I into R248 (Rb) and R249 (Ra), then Rd = (R249 << 8) + R248.
        CHECK(std::count_if(out.begin(), out.end(),
                            [](u64 i) { return (i >> 48) == 0x5cb0; }) == 2);
        CHECK(std::find(out.begin(), out.end(), expected) != out.end());
    }
}

TEST_CASE("Ampere LEA with a constant base and no carry out lowers to ISCADD", "[video_core][ampere]") {
    // LEA R0, R0, c[0][0x2e0]-style constant, 0x8 from Pokemon Pokopia's album compute shaders.
    const std::array<Instruction, 2> code{{{{0x00007a11, 0x01002e00, 0x078e40ff, 0x000fe200}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    // ISCADD R0, R0, R251, 8 with the constant loaded into R251, and no carry compare.
    const auto& out = lowered.instructions;
    REQUIRE(std::find(out.begin(), out.end(), 0x5c1804000fb70000ULL) != out.end());
    REQUIRE(std::none_of(out.begin(), out.end(), [](u64 i) { return (i >> 48) == 0x5b60; }));
}

TEST_CASE("Ampere SHF.L.U32.HI immediate without .W lowers to a clamping SHL", "[video_core][ampere]") {
    // SHF.L.U32.HI R16, RZ, R14, 0xff from Pokemon Pokopia's photo compute shaders.
    const std::array<Instruction, 2> code{{{{0xff107419, 0x000000ff, 0x0001060e, 0x080fe400}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    // SHL R16, R250, R14 without .W (bit 39), so a shift of 32 or more gives zero.
    const auto& out = lowered.instructions;
    REQUIRE(std::find(out.begin(), out.end(), 0x5c48000000e7fa10ULL) != out.end());
}

TEST_CASE("Ampere guarded UIADD3 lowers to a guarded IADD32I or IADD", "[video_core][ampere]") {
    // @!UP0 UIADD3 UR6, UR14, 0x1c, URZ from Pokemon Pokopia's photo compute shaders.
    // @UP0 UIADD3 UR6, UR6, UR7, URZ from the same shaders (UR second source).
    struct Case { u32 guarded, plain, guard; };
    // @!UP1 UIADD3 UR6, UR9, UR8, UR6 (a third source: two guarded adds).
    struct Words { u32 b, w2, w3; };
    const auto words = [](u32 guarded) {
        return guarded == 0x0e068890 ? Words{0x1c, 0x0fffe03f, 0x000fcc00}
             : guarded == 0x06060290 ? Words{0x7, 0x0fffe03f, 0x000fc800}
                                     : Words{0x8, 0x0fffe006, 0x000fc800};
    };
    for (const Case c : {Case{0x0e068890, 0x0e067890, 0xe}, Case{0x06060290, 0x06067290, 0x6},
                         Case{0x09069290, 0x09067290, 0xd}}) {
        const Words w = words(c.guarded);
        const std::array<Instruction, 2> guarded{{{{c.guarded, w.b, w.w2, w.w3}}, Exit}};
        const std::array<Instruction, 2> plain{{{{c.plain, w.b, w.w2, w.w3}}, Exit}};
        const auto lowered = LowerToMaxwell(guarded);
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        const auto reference = LowerToMaxwell(plain);
        REQUIRE(static_cast<bool>(reference));
        // The same add as the unguarded one, predicated on P6 (negated for @!UP0) where UP0 is swapped in.
        // Every add of the unguarded lowering appears again, predicated on the swapped-in guard.
        const auto& out = lowered.instructions;
        for (const u64 add : reference.instructions) {
            if ((add >> 52) == 0xe30) continue; // EXIT
            const u64 guarded_add = (add & ~(u64{0xf} << 16)) | (u64{c.guard} << 16);
            REQUIRE(std::find(out.begin(), out.end(), guarded_add) != out.end());
        }
    }
}

TEST_CASE("Ampere DEPBAR.LE preserves shared visibility for SB0 wait lists", "[video_core][ampere]") {
    // DEPBAR.LE SB5, 0x1 and DEPBAR.LE SB0, 0x0, {3,2,1} (Pokemon Pokopia's photo compute shaders).
    for (const u32 word : {0x0000d040U, 0x0000800eU, 0x00008000U, 0x00008fffU, 0x0000ffffU}) {
        const std::array<Instruction, 2> code{{{{0x0000791a, word, 0, 0x000fce00}}, Exit}};
        const auto lowered = LowerToMaxwell(code);
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.instructions.size() == 1);
        REQUIRE(lowered.instructions.back() == 0xe30000000007000fULL);
        const Instruction copy{{0x02067fae, 0, 0x0f901c08, 0}};
        const auto copying = LowerToMaxwell(std::array{copy, code[0], Exit});
        INFO(copying.error);
        REQUIRE(copying);
        const bool shared_wait = (word & 0x7000U) == 0;
        REQUIRE(std::count(copying.instructions.begin(), copying.instructions.end(),
                           0xef98000000070000ULL) == (shared_wait ? 1 : 0));
        REQUIRE(copying.source_instruction_offsets[2] - copying.source_instruction_offsets[1] ==
                (shared_wait ? 1 : 0));
        if (shared_wait) {
            REQUIRE(copying.instructions[copying.source_instruction_offsets[1]] ==
                    0xef98000000070000ULL);
        }
        REQUIRE(copying.instructions.back() == 0xe30000000007000fULL);
    }
}

TEST_CASE("Ampere guarded USHF.R.U32.HI lowers to a guarded SHR", "[video_core][ampere]") {
    // @UP0 USHF.R.U32.HI UR7, URZ, 0x2, UR6 from Pokemon Pokopia's photo compute shaders.
    const std::array<Instruction, 2> guarded{{{{0x3f070899, 0x2, 0x08011606, 0x000fcc00}}, Exit}};
    const std::array<Instruction, 2> plain{{{{0x3f077899, 0x2, 0x08011606, 0x000fcc00}}, Exit}};
    const auto lowered = LowerToMaxwell(guarded);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const auto reference = LowerToMaxwell(plain);
    REQUIRE(static_cast<bool>(reference));
    const u64 shift = reference.instructions.front();
    const u64 guarded_shift = (shift & ~(u64{0xf} << 16)) | (u64{6} << 16);
    const auto& out = lowered.instructions;
    REQUIRE(std::find(out.begin(), out.end(), guarded_shift) != out.end());
}
} // namespace
