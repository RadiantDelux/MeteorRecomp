#include "mod_resource_resolver.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void WriteLe32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset + 0] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
}

static uint32_t ReadLe32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset + 0]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

static void WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(output.good());
}

static void WriteText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    assert(output.good());
}

static void SetModRoot(const fs::path& path) {
#ifdef _WIN32
    const std::string text = path.string();
    assert(_putenv_s("METEOR_MOD_ROOT", text.c_str()) == 0);
#else
    assert(setenv("METEOR_MOD_ROOT", path.string().c_str(), 1) == 0);
#endif
}

int main() {
    const fs::path root = fs::temp_directory_path() / "meteor_mod_resource_resolver_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    constexpr uint32_t kSlotOffset = 0x800;
    constexpr uint32_t kSlotSize = 8;
    constexpr uint32_t kSecondSlotOffset = 0x900;
    constexpr uint32_t kSecondSlotSize = 4;
    std::vector<uint8_t> afs(kSecondSlotOffset + kSecondSlotSize, 0xCC);
    afs[0] = 'A';
    afs[1] = 'F';
    afs[2] = 'S';
    afs[3] = 0;
    WriteLe32(afs, 4, 2);
    WriteLe32(afs, 8, kSlotOffset);
    WriteLe32(afs, 12, kSlotSize);
    WriteLe32(afs, 16, kSecondSlotOffset);
    WriteLe32(afs, 20, kSecondSlotSize);
    for (uint32_t i = 0; i < kSlotSize; ++i) {
        afs[kSlotOffset + i] = static_cast<uint8_t>(0x40 + i);
    }
    for (uint32_t i = 0; i < kSecondSlotSize; ++i) {
        afs[kSecondSlotOffset + i] = static_cast<uint8_t>(0x70 + i);
    }

    const fs::path afsPath = root / "retail" / "test.afs";
    WriteFile(afsPath, afs);
    const fs::path plainPath = root / "retail" / "plain.bin";
    WriteFile(plainPath, {0, 1, 2, 3, 4, 5, 6, 7});
    const fs::path tinyPath = root / "retail" / "tiny.bin";
    WriteFile(tinyPath, {4, 3, 2, 1});
    const fs::path resizedPath = root / "retail" / "resized.bin";
    WriteFile(resizedPath, {0, 0, 0, 0});

    std::vector<uint8_t> virtualAfs(0x2000, 0xCC);
    virtualAfs[0] = 'A';
    virtualAfs[1] = 'F';
    virtualAfs[2] = 'S';
    virtualAfs[3] = 0;
    WriteLe32(virtualAfs, 4, 2);
    WriteLe32(virtualAfs, 8, 0x800);
    WriteLe32(virtualAfs, 12, 100);
    WriteLe32(virtualAfs, 16, 0x1000);
    WriteLe32(virtualAfs, 20, 8);
    for (size_t i = 0; i < 100; ++i) {
        virtualAfs[0x800 + i] = 0x21;
    }
    const std::array<uint8_t, 8> secondRetail = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8};
    std::copy(secondRetail.begin(), secondRetail.end(), virtualAfs.begin() + 0x1000);
    const fs::path virtualAfsPath = root / "retail" / "virtual.afs";
    WriteFile(virtualAfsPath, virtualAfs);
    const fs::path growAfsPath = root / "retail" / "grow.afs";
    WriteFile(growAfsPath, virtualAfs);

    const fs::path low = root / "low";
    WriteFile(low / "payload.bin", {1, 2, 3, 4});
    WriteFile(low / "file.bin", {1, 1, 1});
    WriteFile(low / "whole_test.afs", afs);
    WriteText(low / "mod.toml",
              "[mod]\n"
              "id = \"low\"\n"
              "name = \"Low priority\"\n"
              "platform = \"wii\"\n"
              "enabled = true\n"
              "priority = 10\n\n"
              "[[file_overrides]]\n"
              "path = \"//PLAIN.BIN/\"\n"
              "payload = \"file.bin\"\n\n"
              "[[file_overrides]]\n"
              "path = \"/test.afs\"\n"
              "payload = \"whole_test.afs\"\n\n"
              "[[afs_overrides]]\n"
              "archive = \"/folder/../TEST.AFS\"\n"
              "index = 0\n"
              "payload = \"payload.bin\"\n");

    const fs::path high = root / "high";
    WriteFile(high / "payload.bin", {9, 8, 7});
    WriteFile(high / "file.bin", {9, 9, 8, 8, 7});
    WriteFile(high / "resized.bin", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    WriteFile(high / "files" / "plain.bin", {2, 2, 2});
    WriteFile(high / "files" / "sub" / "tree.bin", {6, 7, 8});
    WriteFile(high / "virtual_payload.bin", std::vector<uint8_t>(3000, 0x5A));
    WriteFile(high / "grow_payload.bin", std::vector<uint8_t>(7000, 0x6B));
    WriteText(high / "mod.toml",
              "[mod]\n"
              "id = \"high\"\n"
              "name = \"High priority\"\n"
              "platform = \"ps2\"\n"
              "enabled = true\n"
              "priority = 20\n"
              "file_root = \"files\"\n\n"
              "[[file_overrides]]\n"
              "path = \"/plain.bin\"\n"
              "payload = \"file.bin\"\n\n"
              "[[file_overrides]]\n"
              "path = \"/resized.bin\"\n"
              "payload = \"resized.bin\"\n"
              "resize = true\n\n"
              "[[afs_overrides]]\n"
              "archive = \"/test.afs\"\n"
              "index = 0\n"
              "payload = \"payload.bin\"\n\n"
              "[[afs_overrides]]\n"
              "archive = \"/virtual.afs\"\n"
              "index = 0\n"
              "payload = \"virtual_payload.bin\"\n"
              "resize = true\n\n"
              "[[afs_overrides]]\n"
              "archive = \"/grow.afs\"\n"
              "index = 0\n"
              "payload = \"grow_payload.bin\"\n"
              "resize = true\n");

    const fs::path disabled = root / "disabled";
    WriteFile(disabled / "payload.bin", {6, 6, 6});
    WriteText(disabled / "mod.toml",
              "[mod]\n"
              "id = \"disabled\"\n"
              "name = \"Disabled\"\n"
              "platform = \"meteor\"\n"
              "enabled = false\n"
              "priority = 999\n\n"
              "[[afs_overrides]]\n"
              "archive = \"/test.afs\"\n"
              "index = 0\n"
              "payload = \"payload.bin\"\n");

    const fs::path oversize = root / "oversize";
    WriteFile(oversize / "payload.bin", {5, 5, 5, 5, 5});
    WriteFile(oversize / "file.bin", {5, 5, 5, 5, 5});
    WriteText(oversize / "mod.toml",
              "[mod]\n"
              "id = \"oversize\"\n"
              "name = \"Oversize\"\n"
              "platform = \"wii\"\n"
              "enabled = true\n"
              "priority = 30\n\n"
              "[[file_overrides]]\n"
              "path = \"/tiny.bin\"\n"
              "payload = \"file.bin\"\n\n"
              "[[afs_overrides]]\n"
              "archive = \"/test.afs\"\n"
              "index = 1\n"
              "payload = \"payload.bin\"\n");

    const fs::path missingDependency = root / "missing-dependency";
    WriteFile(missingDependency / "payload.bin", {4, 4, 4});
    WriteText(missingDependency / "mod.toml",
              "[mod]\n"
              "id = \"missing-dependency\"\n"
              "name = \"Missing dependency\"\n"
              "platform = \"wii\"\n"
              "enabled = true\n"
              "priority = 1000\n"
              "requires = [\"not-installed\"]\n\n"
              "[[afs_overrides]]\n"
              "archive = \"/test.afs\"\n"
              "index = 0\n"
              "payload = \"payload.bin\"\n");

    const fs::path conflicting = root / "conflicting";
    WriteFile(conflicting / "payload.bin", {3, 3, 3});
    WriteText(conflicting / "mod.toml",
              "[mod]\n"
              "id = \"conflicting\"\n"
              "name = \"Conflicting\"\n"
              "platform = \"meteor\"\n"
              "enabled = true\n"
              "priority = 5\n"
              "conflicts = [\"high\"]\n\n"
              "[[afs_overrides]]\n"
              "archive = \"/test.afs\"\n"
              "index = 0\n"
              "payload = \"payload.bin\"\n");

    // Regression for dependency/conflict ordering. dependent-blocker initially
    // outranks and conflicts with revived, but its required dependency loses a
    // separate higher-priority conflict. The blocker must then be removed and
    // revived must be reconsidered/activated.
    const fs::path dependencyWinner = root / "dependency-winner";
    WriteText(dependencyWinner / "mod.toml",
              "[mod]\n"
              "id = \"dependency-winner\"\n"
              "name = \"Dependency Winner\"\n"
              "platform = \"meteor\"\n"
              "enabled = true\n"
              "priority = 200\n");

    const fs::path dependencyLoser = root / "dependency-loser";
    WriteText(dependencyLoser / "mod.toml",
              "[mod]\n"
              "id = \"dependency-loser\"\n"
              "name = \"Dependency Loser\"\n"
              "platform = \"meteor\"\n"
              "enabled = true\n"
              "priority = 100\n"
              "conflicts = [\"dependency-winner\"]\n");

    const fs::path dependentBlocker = root / "dependent-blocker";
    WriteText(dependentBlocker / "mod.toml",
              "[mod]\n"
              "id = \"dependent-blocker\"\n"
              "name = \"Dependent Blocker\"\n"
              "platform = \"meteor\"\n"
              "enabled = true\n"
              "priority = 500\n"
              "requires = [\"dependency-loser\"]\n"
              "conflicts = [\"revived\"]\n");

    const fs::path revived = root / "revived";
    WriteFile(revived / "revived.bin", {8, 6, 4, 2});
    WriteText(revived / "mod.toml",
              "[mod]\n"
              "id = \"revived\"\n"
              "name = \"Revived\"\n"
              "platform = \"wii\"\n"
              "enabled = true\n"
              "priority = 10\n\n"
              "[[file_overrides]]\n"
              "path = \"/revived.bin\"\n"
              "payload = \"revived.bin\"\n");

    const fs::path stateDisabled = root / "state-disabled";
    WriteText(stateDisabled / "mod.toml",
              "[mod]\n"
              "id = \"state-disabled\"\n"
              "name = \"State Disabled\"\n"
              "platform = \"wii\"\n"
              "enabled = true\n"
              "priority = 50\n");

    const fs::path implicitWii = root / "implicit-wii";
    WriteFile(implicitWii / "files" / "implicit.bin", {1, 3, 5, 7});

    WriteFile(root / "outside.bin", {9, 9, 9});
    const fs::path escaping = root / "escaping";
    WriteText(escaping / "mod.toml",
              "[mod]\n"
              "id = \"escaping\"\n"
              "name = \"Escaping\"\n"
              "platform = \"wii\"\n"
              "enabled = true\n"
              "priority = 1\n\n"
              "[[file_overrides]]\n"
              "path = \"/escape.bin\"\n"
              "payload = \"../outside.bin\"\n");

    const fs::path outsideTree = root / "outside-tree";
    WriteFile(outsideTree / "leak.bin", {7, 7, 7, 7});

    bool symlinkPayloadCreated = false;
    const fs::path symlinkPayload = root / "symlink-payload";
    fs::create_directories(symlinkPayload);
    {
        std::error_code symlinkEc;
        fs::create_directory_symlink(outsideTree, symlinkPayload / "linked", symlinkEc);
        symlinkPayloadCreated = !symlinkEc;
    }
    if (symlinkPayloadCreated) {
        WriteText(symlinkPayload / "mod.toml",
                  "[mod]\n"
                  "id = \"symlink-payload\"\n"
                  "name = \"Symlink Payload\"\n"
                  "platform = \"wii\"\n"
                  "enabled = true\n"
                  "priority = 2\n\n"
                  "[[file_overrides]]\n"
                  "path = \"/symlink-payload.bin\"\n"
                  "payload = \"linked/leak.bin\"\n");
    }

    bool symlinkFileRootCreated = false;
    const fs::path symlinkFileRoot = root / "symlink-file-root";
    fs::create_directories(symlinkFileRoot);
    {
        std::error_code symlinkEc;
        fs::create_directory_symlink(outsideTree, symlinkFileRoot / "files", symlinkEc);
        symlinkFileRootCreated = !symlinkEc;
    }
    if (symlinkFileRootCreated) {
        WriteText(symlinkFileRoot / "mod.toml",
                  "[mod]\n"
                  "id = \"symlink-file-root\"\n"
                  "name = \"Symlink File Root\"\n"
                  "platform = \"wii\"\n"
                  "enabled = true\n"
                  "priority = 2\n"
                  "file_root = \"files\"\n");
    }

    WriteText(root / "state.toml",
              "[[mods]]\n"
              "id = \"state-disabled\"\n"
              "enabled = false\n\n"
              "[[mods]]\n"
              "id = \"implicit-wii\"\n"
              "enabled = true\n"
              "priority = 12\n");

    SetModRoot(root);

    std::vector<uint8_t> whole = afs;
    RuntimeMods::ApplyDvdReadOverlays("/test.afs", afsPath, 0, whole);
    assert(whole[0] == 'A' && whole[1] == 'F' && whole[2] == 'S' && whole[3] == 0);
    assert(whole[kSlotOffset + 0] == 9);
    assert(whole[kSlotOffset + 1] == 8);
    assert(whole[kSlotOffset + 2] == 7);
    for (uint32_t i = 3; i < kSlotSize; ++i) {
        assert(whole[kSlotOffset + i] == 0);
    }
    for (uint32_t i = 0; i < kSecondSlotSize; ++i) {
        assert(whole[kSecondSlotOffset + i] == static_cast<uint8_t>(0x70 + i));
    }

    std::vector<uint8_t> partial = {0xAA, 0xAA, 0xAA, 0xAA};
    RuntimeMods::ApplyDvdReadOverlays("/test.afs", afsPath, kSlotOffset + 1, partial);
    assert((partial == std::vector<uint8_t>{8, 7, 0, 0}));

    std::vector<uint8_t> plainWhole = {0, 1, 2, 3, 4, 5, 6, 7};
    RuntimeMods::ApplyDvdReadOverlays("/plain.bin", plainPath, 0, plainWhole);
    assert((plainWhole == std::vector<uint8_t>{9, 9, 8, 8, 7, 0, 0, 0}));

    std::vector<uint8_t> plainPartial = {0xAA, 0xAA, 0xAA, 0xAA};
    RuntimeMods::ApplyDvdReadOverlays("/plain.bin", plainPath, 2, plainPartial);
    assert((plainPartial == std::vector<uint8_t>{8, 8, 7, 0}));

    const fs::path treeRetailPath = root / "retail" / "sub" / "tree.bin";
    WriteFile(treeRetailPath, {0, 0, 0, 0});
    std::vector<uint8_t> tree = {0, 0, 0, 0};
    RuntimeMods::ApplyDvdReadOverlays("/sub/tree.bin", treeRetailPath, 0, tree);
    assert((tree == std::vector<uint8_t>{6, 7, 8, 0}));

    std::vector<uint8_t> tiny = {4, 3, 2, 1};
    RuntimeMods::ApplyDvdReadOverlays("/tiny.bin", tinyPath, 0, tiny);
    assert((tiny == std::vector<uint8_t>{4, 3, 2, 1}));

    const auto resizedSize = RuntimeMods::GetResizedFileSize("/resized.bin");
    assert(resizedSize && *resizedSize == 10);
    assert(!RuntimeMods::GetResizedFileSize("/plain.bin"));
    std::vector<uint8_t> resizedRead;
    assert(RuntimeMods::ReadFileOverride("/resized.bin", 4, 4, resizedRead) ==
           RuntimeMods::FileOverrideReadResult::Success);
    assert((resizedRead == std::vector<uint8_t>{4, 5, 6, 7}));
    assert(RuntimeMods::ReadFileOverride("/resized.bin", 9, 2, resizedRead) ==
           RuntimeMods::FileOverrideReadResult::Failure);

    std::vector<uint8_t> virtualHeader(24);
    {
        std::ifstream input(virtualAfsPath, std::ios::binary);
        input.read(reinterpret_cast<char*>(virtualHeader.data()), static_cast<std::streamsize>(virtualHeader.size()));
        assert(input.good());
    }
    RuntimeMods::ApplyDvdReadOverlays("/virtual.afs", virtualAfsPath, 0, virtualHeader);
    assert(ReadLe32(virtualHeader, 8) == 0x800);
    assert(ReadLe32(virtualHeader, 12) == 3000);
    assert(ReadLe32(virtualHeader, 16) == 0x1800);
    assert(ReadLe32(virtualHeader, 20) == 8);

    std::vector<uint8_t> resizedPayload(16, 0);
    RuntimeMods::ApplyDvdReadOverlays("/virtual.afs", virtualAfsPath, 0x800, resizedPayload);
    assert(std::all_of(resizedPayload.begin(), resizedPayload.end(), [](uint8_t value) { return value == 0x5A; }));

    std::vector<uint8_t> shiftedRetail(8, 0);
    RuntimeMods::ApplyDvdReadOverlays("/virtual.afs", virtualAfsPath, 0x1800, shiftedRetail);
    assert(std::equal(shiftedRetail.begin(), shiftedRetail.end(), secondRetail.begin()));

    // A virtual AFS is also a direct file provider when its repacked data grows
    // past retail EOF. Entry 0 ends at 0x2358, entry 1 shifts to 0x2800 and the
    // published file therefore grows from 0x2000 to 0x2808.
    assert(!RuntimeMods::GetResizedFileSize("/virtual.afs", virtualAfsPath));
    const auto grownAfsSize = RuntimeMods::GetResizedFileSize("/grow.afs", growAfsPath);
    assert(grownAfsSize && *grownAfsSize == 0x2808);

    std::vector<uint8_t> grownHeader;
    assert(RuntimeMods::ReadVirtualAfs("/grow.afs", growAfsPath, 0, 24, grownHeader) ==
           RuntimeMods::FileOverrideReadResult::Success);
    assert(ReadLe32(grownHeader, 8) == 0x800);
    assert(ReadLe32(grownHeader, 12) == 7000);
    assert(ReadLe32(grownHeader, 16) == 0x2800);
    assert(ReadLe32(grownHeader, 20) == 8);

    std::vector<uint8_t> grownPayload;
    assert(RuntimeMods::ReadVirtualAfs("/grow.afs", growAfsPath, 0x2200, 16, grownPayload) ==
           RuntimeMods::FileOverrideReadResult::Success);
    assert(std::all_of(grownPayload.begin(), grownPayload.end(), [](uint8_t value) { return value == 0x6B; }));

    std::vector<uint8_t> grownPadding;
    assert(RuntimeMods::ReadVirtualAfs("/grow.afs", growAfsPath, 0x2400, 16, grownPadding) ==
           RuntimeMods::FileOverrideReadResult::Success);
    assert(std::all_of(grownPadding.begin(), grownPadding.end(), [](uint8_t value) { return value == 0; }));

    std::vector<uint8_t> grownShiftedRetail;
    assert(RuntimeMods::ReadVirtualAfs("/grow.afs", growAfsPath, 0x2800, 8, grownShiftedRetail) ==
           RuntimeMods::FileOverrideReadResult::Success);
    assert(std::equal(grownShiftedRetail.begin(), grownShiftedRetail.end(), secondRetail.begin()));
    assert(RuntimeMods::ReadVirtualAfs("/grow.afs", growAfsPath, 0x2808, 1, grownShiftedRetail) ==
           RuntimeMods::FileOverrideReadResult::Failure);

    const std::vector<RuntimeMods::ModStatus> statuses = RuntimeMods::GetModStatuses();
    auto status = [&](const char* id) -> const RuntimeMods::ModStatus& {
        const auto found = std::find_if(statuses.begin(), statuses.end(),
                                        [&](const RuntimeMods::ModStatus& value) { return value.id == id; });
        assert(found != statuses.end());
        return *found;
    };
    assert(status("high").active && status("high").enabled);
    assert(status("high").platform == "ps2");
    assert(status("high").fileOverrideCount == 4);
    assert(status("high").afsOverrideCount == 3);
    assert(!status("disabled").active && !status("disabled").enabled);
    assert(status("disabled").reason == "disabled");
    assert(!status("conflicting").active);
    assert(status("conflicting").reason == "conflicts with high");
    assert(!status("missing-dependency").active);
    assert(status("missing-dependency").reason == "requires not-installed");
    assert(status("dependency-winner").active);
    assert(!status("dependency-loser").active);
    assert(status("dependency-loser").reason == "conflicts with dependency-winner");
    assert(!status("dependent-blocker").active);
    assert(status("dependent-blocker").reason == "requires dependency-loser");
    assert(status("revived").active);
    assert(!status("state-disabled").active);
    assert(!status("state-disabled").enabled);
    assert(status("state-disabled").reason == "disabled");
    assert(status("implicit-wii").active);
    assert(status("implicit-wii").enabled);
    assert(status("implicit-wii").platform == "wii");
    assert(status("implicit-wii").priority == 12);
    assert(status("implicit-wii").fileOverrideCount == 1);
    assert(status("escaping").active);
    assert(status("escaping").fileOverrideCount == 0);
    if (symlinkPayloadCreated) {
        assert(status("symlink-payload").active);
        assert(status("symlink-payload").fileOverrideCount == 0);
    }
    if (symlinkFileRootCreated) {
        assert(status("symlink-file-root").active);
        assert(status("symlink-file-root").fileOverrideCount == 0);
    }

    const std::vector<RuntimeMods::FileOverrideRegistration> registrations =
        RuntimeMods::GetFileOverrideRegistrations();
    const auto treeRegistration = std::find_if(
        registrations.begin(), registrations.end(),
        [](const RuntimeMods::FileOverrideRegistration& value) {
            return value.dvdPath == "/sub/tree.bin";
        });
    assert(treeRegistration != registrations.end());
    assert(treeRegistration->payloadSize == 3);
    const auto resizedRegistration = std::find_if(
        registrations.begin(), registrations.end(),
        [](const RuntimeMods::FileOverrideRegistration& value) {
            return value.dvdPath == "/resized.bin";
        });
    assert(resizedRegistration != registrations.end());
    assert(resizedRegistration->resize);
    assert(resizedRegistration->payloadSize == 10);
    const auto implicitRegistration = std::find_if(
        registrations.begin(), registrations.end(),
        [](const RuntimeMods::FileOverrideRegistration& value) {
            return value.dvdPath == "/implicit.bin";
        });
    assert(implicitRegistration != registrations.end());
    assert(implicitRegistration->resize);
    assert(implicitRegistration->payloadSize == 4);
    const auto revivedRegistration = std::find_if(
        registrations.begin(), registrations.end(),
        [](const RuntimeMods::FileOverrideRegistration& value) {
            return value.dvdPath == "/revived.bin";
        });
    assert(revivedRegistration != registrations.end());
    assert(revivedRegistration->payloadSize == 4);
    const auto escapingRegistration = std::find_if(
        registrations.begin(), registrations.end(),
        [](const RuntimeMods::FileOverrideRegistration& value) {
            return value.dvdPath == "/escape.bin";
        });
    assert(escapingRegistration == registrations.end());
    const auto symlinkPayloadRegistration = std::find_if(
        registrations.begin(), registrations.end(),
        [](const RuntimeMods::FileOverrideRegistration& value) {
            return value.dvdPath == "/symlink-payload.bin";
        });
    assert(symlinkPayloadRegistration == registrations.end());
    if (symlinkFileRootCreated) {
        const auto symlinkRootRegistration = std::find_if(
            registrations.begin(), registrations.end(),
            [](const RuntimeMods::FileOverrideRegistration& value) {
                return value.dvdPath == "/leak.bin";
            });
        assert(symlinkRootRegistration == registrations.end());
    }

    assert(RuntimeMods::SetModEnabledForNextLaunch("high", false));
    assert(RuntimeMods::SetModPriorityForNextLaunch("high", 77));
    const std::vector<RuntimeMods::ModStatus> pendingStatuses = RuntimeMods::GetModStatuses();
    const auto pendingHigh = std::find_if(pendingStatuses.begin(), pendingStatuses.end(),
                                          [](const RuntimeMods::ModStatus& value) { return value.id == "high"; });
    assert(pendingHigh != pendingStatuses.end());
    assert(pendingHigh->active);
    assert(!pendingHigh->enabled);
    assert(pendingHigh->priority == 77);
    assert(pendingHigh->reason == "restart required");

    const fs::path statePath = RuntimeMods::ModStatePath();
    assert(fs::is_regular_file(statePath));
    std::ifstream stateFile(statePath, std::ios::binary);
    const std::string stateText((std::istreambuf_iterator<char>(stateFile)), std::istreambuf_iterator<char>());
    assert(stateText.find("id = \"high\"") != std::string::npos);
    assert(stateText.find("enabled = false") != std::string::npos);
    assert(stateText.find("priority = 77") != std::string::npos);
    assert(stateText.find("id = \"state-disabled\"") != std::string::npos);
    assert(stateText.find("id = \"implicit-wii\"") != std::string::npos);

    // A failed publish must not leak the attempted value into the in-memory
    // user state, otherwise a later successful UI change could persist a value
    // that the user was told failed. Replacing state.toml with a directory
    // forces the atomic rename to fail without relying on host permissions.
    stateFile.close();
    fs::remove(statePath, ec);
    assert(!ec);
    fs::create_directory(statePath, ec);
    assert(!ec);
    assert(!RuntimeMods::SetModPriorityForNextLaunch("high", 88));
    const std::vector<RuntimeMods::ModStatus> failedStatuses = RuntimeMods::GetModStatuses();
    const auto failedHigh = std::find_if(failedStatuses.begin(), failedStatuses.end(),
                                         [](const RuntimeMods::ModStatus& value) { return value.id == "high"; });
    assert(failedHigh != failedStatuses.end());
    assert(failedHigh->priority == 77);
    fs::remove_all(statePath, ec);
    assert(!ec);
    assert(RuntimeMods::SetModEnabledForNextLaunch("high", true));

    std::ifstream recoveredStateFile(statePath, std::ios::binary);
    const std::string recoveredStateText((std::istreambuf_iterator<char>(recoveredStateFile)),
                                         std::istreambuf_iterator<char>());
    assert(recoveredStateText.find("priority = 77") != std::string::npos);
    assert(recoveredStateText.find("priority = 88") == std::string::npos);

    for (const fs::directory_entry& entry : fs::directory_iterator(root)) {
        const std::string filename = entry.path().filename().string();
        assert(filename.rfind("state.toml.tmp.", 0) != 0);
    }

    fs::remove_all(root, ec);
    return 0;
}
