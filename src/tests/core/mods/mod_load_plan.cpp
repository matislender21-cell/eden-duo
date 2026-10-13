// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "common/hex_util.h"
#include "core/mods/mod_load_plan.h"
#include "core/mods/mod_package_io.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/mods/mod_runtime.h"

namespace Core::Mods {
namespace {

std::vector<u8> Words(std::initializer_list<u32> words) {
    std::vector<u8> bytes(words.size() * sizeof(u32));
    std::memcpy(bytes.data(), words.begin(), bytes.size());
    return bytes;
}

constexpr u64 PlanTitle = 0x0100ABCD12340000;

std::array<u8, 0x20> BuildId(u8 first) {
    std::array<u8, 0x20> id{};
    id[0] = first;
    id[1] = 0x5E;
    return id;
}

LoadPlanLayout PlanLayout() {
    LoadPlanLayout layout;
    layout.segments = {{{0, 0x1000}, {0x1000, 0x1000}, {0x2000, 0x1000}}};
    layout.bss_size = 0x1000;
    return layout;
}

nlohmann::json PlanJson(const std::array<u8, 0x20>& build) {
    return {
        {"format", 1},
        {"module", "main"},
        {"title_id", "0100ABCD12340000"},
        {"build_id", Common::HexToString(build, true)},
        {"layout",
         {{"segments",
           {{{"location", 0}, {"size", 0x1000}},
            {{"location", 0x1000}, {"size", 0x1000}},
            {{"location", 0x2000}, {"size", 0x1000}}}},
          {"bss_size", 0x1000}}},
        {"mailbox_size", 0x1000},
        {"patches", {{{"offset", 0x100}, {"expected", "1F2003D5"}, {"replacement", "C0035FD6"}}}},
    };
}

/// A package's dualscreen folder with guest/<build>/load-plan.json for each given plan.
FileSys::VirtualDir PlanAssets(const std::vector<std::pair<std::string, std::string>>& plans) {
    std::vector<FileSys::VirtualDir> builds;
    for (const auto& [build, text] : plans) {
        auto file = std::make_shared<FileSys::VectorVfsFile>(
            std::vector<u8>(text.begin(), text.end()), "load-plan.json");
        builds.push_back(std::make_shared<FileSys::VectorVfsDirectory>(
            std::vector<FileSys::VirtualFile>{file}, std::vector<FileSys::VirtualDir>{}, build));
    }
    auto guest = std::make_shared<FileSys::VectorVfsDirectory>(std::vector<FileSys::VirtualFile>{},
                                                               std::move(builds), "guest");
    return std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{guest}, "dualscreen");
}

const nlohmann::json PlanManifest{{"load_plan", "guest/<build>/load-plan.json"}};

/// A main image whose plan site holds the bytes the plan expects (NOP).
std::vector<u8> PlanImage() {
    std::vector<u8> image(0x4000);
    const auto nop = Words({0xD503201F});
    std::copy(nop.begin(), nop.end(), image.begin() + 0x100);
    return image;
}

} // namespace

TEST_CASE("DSMod guest helper runtime boundary gates older hosts", "[dsmod][load-plan]") {
    const auto manifest = nlohmann::json{{"min_runtime", 18}};
    const auto package = nlohmann::json{{"min_runtime", 19}};
    const u32 required = PackageMinRuntime(&manifest, &package);
    REQUIRE(required == GuestHelperMinimumRuntime);
    REQUIRE_FALSE(IsLoadPlanRuntimeCompatible(2, required, 18));
    REQUIRE(IsLoadPlanRuntimeCompatible(2, required, DualScreenRuntimeVersion));
    REQUIRE_FALSE(IsLoadPlanRuntimeCompatible(2, 18, DualScreenRuntimeVersion));
    REQUIRE_FALSE(IsLoadPlanRuntimeCompatible(2, 0, DualScreenRuntimeVersion));
    REQUIRE_FALSE(IsLoadPlanRuntimeCompatible(2, DualScreenRuntimeVersion + 1, DualScreenRuntimeVersion));
    REQUIRE(IsLoadPlanRuntimeCompatible(1, 18, 18));
    REQUIRE(IsLoadPlanRuntimeCompatible(1, 0, DualScreenRuntimeVersion));
    REQUIRE_FALSE(IsLoadPlanRuntimeCompatible(3, 19, DualScreenRuntimeVersion));
}

TEST_CASE("DSMod load plan applies mailbox relocations", "[dsmod][load-plan]") {
    constexpr VAddr LoadBase = 0x100000;
    constexpr VAddr Mailbox = 0x23456000;
    constexpr u64 PayloadOffset = 0x100;
    std::vector<u8> image(0x200);
    const auto instructions = Words({0x90000005, 0x910000A5}); // ADRP x5; ADD x5, x5, #0

    ModLoadPlan::Write payload{
        .offset = PayloadOffset,
        .expected = std::vector<u8>(instructions.size()),
        .replacement = instructions,
        .relocations = {{0, ModLoadPlan::RelocationKind::AArch64Adrp, 0},
                        {4, ModLoadPlan::RelocationKind::AArch64AddLo12, 0}},
    };
    const ModLoadPlan plan{0x1000, {std::move(payload)}};

    REQUIRE(plan.Apply(image, LoadBase, Mailbox));
    u32 adrp{}, add{};
    std::memcpy(&adrp, image.data() + PayloadOffset, sizeof(adrp));
    std::memcpy(&add, image.data() + PayloadOffset + 4, sizeof(add));
    const u64 encoded = ((adrp >> 29) & 3) | (((adrp >> 5) & 0x7FFFF) << 2);
    const s64 pages = static_cast<s64>(encoded << 43) >> 43;
    const VAddr decoded_page = ((LoadBase + PayloadOffset) & ~0xFFFULL) + pages * 0x1000;
    REQUIRE(decoded_page == (Mailbox & ~0xFFFULL));
    REQUIRE(((add >> 10) & 0xFFF) == (Mailbox & 0xFFF));
}

TEST_CASE("DSMod load plan validation is transactional", "[dsmod][load-plan]") {
    std::vector<u8> image{1, 2, 3, 4};
    const auto original = image;
    ModLoadPlan plan{
        0x1000,
        {{.offset = 0, .expected = {1}, .replacement = {9}},
         {.offset = 3, .expected = {8}, .replacement = {7}}},
    };

    REQUIRE_FALSE(plan.Apply(image, 0x1000, 0x4000));
    REQUIRE(image == original);
}

TEST_CASE("DSMod load plan rejects malformed relocation and write bounds", "[dsmod][load-plan]") {
    std::vector<u8> image(16);
    auto replacement = Words({0x910000A5});
    ModLoadPlan::Write write{
        .offset = 0, .expected = std::vector<u8>(4), .replacement = replacement};
    SECTION("replacement cannot exceed checked region") {
        write.replacement.resize(20);
    }
    SECTION("relocation must stay within payload") {
        write.relocations = {{4, ModLoadPlan::RelocationKind::AArch64AddLo12, 0}};
    }
    SECTION("relocation cannot address outside mailbox") {
        write.relocations = {{0, ModLoadPlan::RelocationKind::AArch64AddLo12, 0x1000}};
    }
    SECTION("ADD relocation must not use shifted immediate") {
        write.replacement = Words({0x914000A5});
        write.relocations = {{0, ModLoadPlan::RelocationKind::AArch64AddLo12, 0}};
    }
    const auto original = image;
    REQUIRE_FALSE(ModLoadPlan{0x1000, {write}}.Apply(image, 0x10000, 0x20000));
    REQUIRE(image == original);
}

TEST_CASE("DSMod mailbox epoch occupies an aligned reserved word", "[dsmod][load-plan]") {
    for (const u32 offset : {0u, 4092u}) {
        std::vector<u8> image{1};
        ModLoadPlan plan{4096, {{.offset = 0, .expected = {1}, .replacement = {2}}}, offset};
        REQUIRE(plan.MailboxEpochOffset() == offset);
        REQUIRE(plan.Apply(image, 0x10000, 0x20000));
    }
    for (const u32 offset : {1u, 4093u, 4096u, 0xFFFFFFFCu}) {
        std::vector<u8> image{1};
        ModLoadPlan plan{4096, {{.offset = 0, .expected = {1}, .replacement = {2}}}, offset};
        REQUIRE_FALSE(plan.Apply(image, 0x10000, 0x20000));
        REQUIRE(image[0] == 1);
    }
    REQUIRE_FALSE(ModLoadPlan{0, {}, 0}.Apply({}, 0x10000, 0x20000));
    REQUIRE_FALSE(ModLoadPlan{3, {}, 0}.Apply({}, 0x10000, 0x20000));
    REQUIRE_FALSE(ModLoadPlan{4096, {}}.MailboxEpochOffset().has_value());
}

TEST_CASE("DSMod loader and runtime share manifest usability rules", "[dsmod][load-plan]") {
    REQUIRE_FALSE(IsUsableDualScreenManifest(nlohmann::json::parse(R"({"pages":[]})")));
    // clang-format off: Catch2 stringifies the expression, keep its original layout
    REQUIRE_FALSE(IsUsableDualScreenManifest(nlohmann::json::parse(
        R"({"pages":[{"widgets":[{"src":17}]}]})")));
    REQUIRE(IsUsableDualScreenManifest(nlohmann::json::parse(
        R"({"pages":[{"id":"race","widgets":[]}]})")));
    // clang-format on
    REQUIRE(IsUsableDualScreenManifest(nlohmann::json::parse(R"({"debug_page":true})")));
}

TEST_CASE("DSMod guest code relocates independently of native and mailbox pages",
          "[dsmod][load-plan]") {
    using Kind = ModLoadPlan::RelocationKind;
    using Target = ModLoadPlan::RelocationTarget;
    constexpr VAddr Main = 0x100000, Mailbox = Main + 0x2000, Code = Main + 0x3000;
    std::vector<u8> image(0x4000);
    ModLoadPlan::Write patch{0x100,
                             std::vector<u8>(4),
                             Words({0x94000000}),
                             {{0, Kind::AArch64Branch26, 0, Target::Code}}};
    ModLoadPlan::GuestCode code{Words({0x90000005, 0x910000A5, 0x94000000, 0xD65F03C0}),
                                {{0, Kind::AArch64Adrp, 4092, Target::Mailbox},
                                 {4, Kind::AArch64AddLo12, 4092, Target::Mailbox},
                                 {8, Kind::AArch64Branch26, 0x180, Target::Main}}};
    const ModLoadPlan plan{4096, {patch}, 4092, code, 0x2000};
    REQUIRE(plan.GuestCodeSize() == 4096);
    REQUIRE(plan.Apply(image, Main, Mailbox, Code));
    const auto word = [&](size_t at) {
        u32 v{};
        std::memcpy(&v, image.data() + at, 4);
        return v;
    };
    const auto branch_target = [](VAddr pc, u32 instruction) {
        const s64 displacement = static_cast<s64>(u64{instruction & 0x03FFFFFF} << 38) >> 36;
        return pc + displacement;
    };
    REQUIRE(branch_target(Main + 0x100, word(0x100)) == Code);
    REQUIRE(branch_target(Code + 8, word(0x3008)) == Main + 0x180);
    REQUIRE(((word(0x3004) >> 10) & 4095) == 4092);
    REQUIRE(word(0x300C) == 0xD65F03C0);
    REQUIRE(std::ranges::all_of(std::span{image}.subspan(0x3010), [](u8 b) { return b == 0; }));
}

TEST_CASE("DSMod guest code validation leaves native and code pages unchanged",
          "[dsmod][load-plan]") {
    using Kind = ModLoadPlan::RelocationKind;
    using Target = ModLoadPlan::RelocationTarget;
    std::vector<u8> image(0x4000);
    ModLoadPlan::Write patch{0x100,
                             std::vector<u8>(4),
                             Words({0x94000000}),
                             {{0, Kind::AArch64Branch26, 0, Target::Code}}};
    ModLoadPlan::GuestCode code{Words({0x94000000, 0xD65F03C0}),
                                {{0, Kind::AArch64Branch26, 0x180, Target::Main}}};
    VAddr code_address = 0x103000;
    SECTION("native original bytes mismatch") {
        image[0x100] = 1;
    }
    SECTION("allocated page must be entirely zero, including padding") {
        image[0x3FFF] = 1;
    }
    SECTION("code relocation cannot point past declared instructions") {
        patch.relocations[0].addend = 8;
    }
    SECTION("native relocation cannot point past original NSO image") {
        code.relocations[0].addend = 0x2000;
    }
    SECTION("code cannot overlap mailbox") {
        code_address = 0x102000;
    }
    SECTION("code requires page alignment") {
        code_address += 4;
    }
    SECTION("code must fit allocated image") {
        code_address = 0x104000;
    }
    SECTION("native patch cannot overlap code payload") {
        patch.offset = 0x3000;
    }
    SECTION("branch relocation rejects non-branch instruction") {
        code.bytes = Words({0, 0});
    }
    SECTION("branch destination requires instruction alignment") {
        code.relocations[0].addend = 1;
    }
    SECTION("relocation budget includes both native patches and helper code") {
        code.relocations.resize(4096, code.relocations[0]);
    }
    SECTION("native patch cannot exceed original NSO image") {
        patch.offset = 0x1FFF;
    }
    SECTION("negative target addends refused") {
        code.relocations[0].addend = -1;
    }
    const auto original = image;
    REQUIRE_FALSE(ModLoadPlan{4096, {patch}, 4092, code, 0x2000}.Apply(image, 0x100000, 0x102000,
                                                                       code_address));
    REQUIRE(image == original);
}

TEST_CASE("DSMod guest code rejects instructions requiring NCE rewriting", "[dsmod][load-plan]") {
    for (const u32 instruction : {0xD4000001u, 0xD4200000u, 0xD53BD040u, 0xD51BD040u}) {
        ModLoadPlan plan{4096, {}, {}, ModLoadPlan::GuestCode{Words({instruction}), {}}};
        REQUIRE(plan.GuestCodeSize() == 0);
    }
    REQUIRE(ModLoadPlan{4096, {}, {}, ModLoadPlan::GuestCode{Words({0xD503201F}), {}}}
                .GuestCodeSize() == 4096);
    REQUIRE(ModLoadPlan{4096, {}, {}, ModLoadPlan::GuestCode{{1, 2, 3}, {}}}.GuestCodeSize() == 0);
    REQUIRE(ModLoadPlan{4096, {}, {}, ModLoadPlan::GuestCode{std::vector<u8>(65540), {}}}
                .GuestCodeSize() == 0);
    REQUIRE(ModLoadPlan{4096, {}}.GuestCodeSize() == 0);
}

TEST_CASE("DSMod guest code relocates only aligned native pointer and handle loads",
          "[dsmod][load-plan]") {
    using Kind = ModLoadPlan::RelocationKind;
    using Target = ModLoadPlan::RelocationTarget;
    std::vector<u8> image(0x4000);
    ModLoadPlan::GuestCode code{Words({0xF9400129, 0xB9400129, 0xD65F03C0}),
                                {{0, Kind::AArch64Ldr64Lo12, 0x188, Target::Main},
                                 {4, Kind::AArch64Ldr32Lo12, 0x184, Target::Main}}};
    SECTION("64-bit pointer and32-bit flag handle") {
        REQUIRE(ModLoadPlan{4096, {}, {}, code, 0x2000}.Apply(image, 0x100000, 0x102000, 0x103000));
        u32 x{}, w{};
        std::memcpy(&x, image.data() + 0x3000, 4);
        std::memcpy(&w, image.data() + 0x3004, 4);
        REQUIRE(((x >> 10) & 4095) * 8 == 0x188);
        REQUIRE(((w >> 10) & 4095) * 4 == 0x184);
    }
    SECTION("misaligned pointer target refuses") {
        code.relocations[0].addend = 0x184;
        REQUIRE_FALSE(
            ModLoadPlan{4096, {}, {}, code, 0x2000}.Apply(image, 0x100000, 0x102000, 0x103000));
    }
    SECTION("store is not silently converted to a load") {
        code.bytes = Words({0xF9000129, 0xB9400129, 0xD65F03C0});
        REQUIRE_FALSE(
            ModLoadPlan{4096, {}, {}, code, 0x2000}.Apply(image, 0x100000, 0x102000, 0x103000));
    }
}

TEST_CASE("DSMod metadata and plans share exact bounded reads", "[dsmod][load-plan]") {
    const auto file = [](size_t size) {
        std::vector<u8> bytes(size, ' ');
        if (size >= 2) {
            bytes[0] = '{';
            bytes[1] = '}';
        }
        return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes));
    };
    REQUIRE(ReadPackageJson(file(MaxLoadPlanBytes + 1)));
    REQUIRE(ReadPackageJson(file(MaxPackageMetadataBytes)));
    REQUIRE_FALSE(ReadPackageJson(file(MaxPackageMetadataBytes + 1)));
    REQUIRE(ReadPackageJson(file(MaxLoadPlanBytes), MaxLoadPlanBytes));
    REQUIRE_FALSE(ReadPackageJson(file(MaxLoadPlanBytes + 1), MaxLoadPlanBytes));
    REQUIRE_FALSE(ReadPackageJson(file(0)));
    REQUIRE_FALSE(ReadPackageJson(nullptr));
    REQUIRE_FALSE(ReadPackageJson(std::make_shared<FileSys::VectorVfsFile>(
        std::vector<u8>{'{', 'x', '}'})));

    class ShortReadFile final : public FileSys::VectorVfsFile {
    public:
        ShortReadFile() : VectorVfsFile(std::vector<u8>{'{', '}', ' '}) {}
        size_t Read(u8* bytes, size_t length, size_t offset) const override {
            return VectorVfsFile::Read(bytes, std::min<size_t>(length, 2), offset);
        }
    };
    // A parseable prefix must not disguise a truncated file.
    REQUIRE_FALSE(ReadPackageJson(std::make_shared<ShortReadFile>()));
}

TEST_CASE("DSMod rejected reload preserves the current manifest", "[dsmod][load-plan][reload]") {
    Manifest current;
    current.title_id = 0x1234;
    current.name = "running module";
    current.mod_dir_name = "selected package";
    current.pages.push_back(Page{.id = "live"});
    Manifest prepared = current;
    for (const auto text : {"[]", "null", R"({"canvas_w":"bad","pages":[]})",
                            R"({"pages":[]})", R"({"format":2,"pages":[{"id":"new"}]})",
                            R"({"title_id":"0000000000004321","pages":[{"id":"new"}]})"}) {
        REQUIRE_FALSE(PrepareDualScreenManifestReload(nlohmann::json::parse(text), current,
                                                     prepared));
        REQUIRE(prepared.name == "running module");
        REQUIRE(prepared.pages.size() == 1);
        REQUIRE(prepared.pages.front().id == "live");
        REQUIRE(current.pages.front().id == "live");
    }
    REQUIRE(PrepareDualScreenManifestReload(
        nlohmann::json::parse(R"({"name":"replacement","pages":[{"id":"new"}]})"), current,
        prepared));
    REQUIRE(prepared.name == "replacement");
    REQUIRE(prepared.mod_dir_name == "selected package");
    REQUIRE(prepared.title_id == current.title_id);
    REQUIRE(prepared.pages.size() == 1);
    REQUIRE(prepared.pages.front().id == "new");
    REQUIRE(current.pages.front().id == "live");
}

TEST_CASE("DSMod valid load plan for the running build is found and applied",
          "[dsmod][load-plan][boot]") {
    const auto build = BuildId(0x4F);
    const auto hex = Common::HexToString(build, true);
    const auto assets = PlanAssets({{hex, PlanJson(build).dump()}});
    const auto discovery =
        ReadModLoadPlan(assets, PlanManifest, 19, PlanTitle, build, "main", PlanLayout());
    REQUIRE(discovery.status == LoadPlanDiscovery::Status::Found);
    REQUIRE(discovery.plan);
    REQUIRE(discovery.build == hex);
    REQUIRE(discovery.plan->MailboxSize() == 0x1000);

    auto image = PlanImage();
    REQUIRE(discovery.plan->Apply(image, 0x80000000, 0x80008000));
    REQUIRE(std::vector<u8>(image.begin() + 0x100, image.begin() + 0x104) ==
            Words({0xD65F03C0}));

    // Other modules of the same executable are not the plan's target.
    REQUIRE(ReadModLoadPlan(assets, PlanManifest, 19, PlanTitle, build, "sdk", PlanLayout())
                .status == LoadPlanDiscovery::Status::None);
}

TEST_CASE("DSMod load plan missing for the running build lets the game start without it",
          "[dsmod][load-plan][boot]") {
    // The package ships a plan for one game version; another version (no update installed, a
    // newer update) has a different build ID and no plan.
    const auto supported = BuildId(0x4F);
    const auto running = BuildId(0x91);
    const auto assets =
        PlanAssets({{Common::HexToString(supported, true), PlanJson(supported).dump()}});
    const auto discovery =
        ReadModLoadPlan(assets, PlanManifest, 19, PlanTitle, running, "main", PlanLayout());
    REQUIRE(discovery.status == LoadPlanDiscovery::Status::Unsupported);
    REQUIRE_FALSE(discovery.plan);
    REQUIRE(discovery.build == Common::HexToString(running, true));
    REQUIRE(discovery.error.find("no load plan for this build") != std::string::npos);

    // A plan stored under the running build that names another build, or another executable
    // layout, is not for this executable either.
    const auto renamed = PlanAssets({{Common::HexToString(running, true), PlanJson(supported).dump()}});
    REQUIRE(ReadModLoadPlan(renamed, PlanManifest, 19, PlanTitle, running, "main", PlanLayout())
                .status == LoadPlanDiscovery::Status::Unsupported);
    auto other_layout = PlanLayout();
    other_layout.segments[1].size = 0x2000;
    const auto exact = PlanAssets({{Common::HexToString(running, true), PlanJson(running).dump()}});
    const auto moved =
        ReadModLoadPlan(exact, PlanManifest, 19, PlanTitle, running, "main", other_layout);
    REQUIRE(moved.status == LoadPlanDiscovery::Status::Unsupported);
    REQUIRE_FALSE(moved.plan);
    auto other_bss = PlanLayout();
    other_bss.bss_size = 0x3000;
    REQUIRE(ReadModLoadPlan(exact, PlanManifest, 19, PlanTitle, running, "main", other_bss)
                .status == LoadPlanDiscovery::Status::Unsupported);

    // A malformed plan for this build is reported (Invalid), never thrown, and carries no plan.
    for (const auto text : {"{", "[]", R"({"module":"main","format":1})"}) {
        const auto broken = PlanAssets({{Common::HexToString(running, true), text}});
        const auto result =
            ReadModLoadPlan(broken, PlanManifest, 19, PlanTitle, running, "main", PlanLayout());
        REQUIRE(result.status != LoadPlanDiscovery::Status::Found);
        REQUIRE_FALSE(result.plan);
    }
    auto bad_writes = PlanJson(running);
    bad_writes["patches"] = nlohmann::json::array();
    const auto empty = PlanAssets({{Common::HexToString(running, true), bad_writes.dump()}});
    const auto invalid =
        ReadModLoadPlan(empty, PlanManifest, 19, PlanTitle, running, "main", PlanLayout());
    REQUIRE(invalid.status == LoadPlanDiscovery::Status::Invalid);
    REQUIRE_FALSE(invalid.plan);
}

TEST_CASE("DSMod load plan whose bytes do not match the executable is not applied",
          "[dsmod][load-plan][boot]") {
    // Same build ID, but the code at the plan site differs (an ExeFS patch, or a mod that
    // replaced the function): Apply refuses and leaves the image untouched, and the loader then
    // starts the game without the plan.
    const auto build = BuildId(0x4F);
    const auto assets = PlanAssets({{Common::HexToString(build, true), PlanJson(build).dump()}});
    const auto discovery =
        ReadModLoadPlan(assets, PlanManifest, 19, PlanTitle, build, "main", PlanLayout());
    REQUIRE(discovery.plan);
    auto image = PlanImage();
    const auto patched = Words({0x94000010}); // BL elsewhere
    std::copy(patched.begin(), patched.end(), image.begin() + 0x100);
    const auto before = image;
    REQUIRE_FALSE(discovery.plan->Apply(image, 0x80000000, 0x80008000));
    REQUIRE(image == before);
}

} // namespace Core::Mods
