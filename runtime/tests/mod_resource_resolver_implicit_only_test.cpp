#include "mod_resource_resolver.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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
    const fs::path root = fs::temp_directory_path() / "meteor_mod_resource_resolver_implicit_only_test";
    std::error_code ec;
    fs::remove_all(root, ec);

    const fs::path implicit = root / "implicit-only";
    WriteFile(implicit / "files" / "new.bin", {1, 2, 3, 4});
    WriteText(root / "state.toml",
              "[[mods]]\n"
              "id = \"implicit-only\"\n"
              "enabled = true\n"
              "priority = 42\n");
    SetModRoot(root);

    const std::vector<RuntimeMods::ModStatus> statuses = RuntimeMods::GetModStatuses();
    const auto status = std::find_if(statuses.begin(), statuses.end(),
                                     [](const RuntimeMods::ModStatus& value) {
                                         return value.id == "implicit-only";
                                     });
    assert(status != statuses.end());
    assert(status->active);
    assert(status->enabled);
    assert(status->platform == "wii");
    assert(status->priority == 42);
    assert(status->fileOverrideCount == 1);

    const std::vector<RuntimeMods::FileOverrideRegistration> registrations =
        RuntimeMods::GetFileOverrideRegistrations();
    const auto registration = std::find_if(
        registrations.begin(), registrations.end(),
        [](const RuntimeMods::FileOverrideRegistration& value) {
            return value.dvdPath == "/new.bin";
        });
    assert(registration != registrations.end());
    assert(registration->payloadSize == 4);
    assert(registration->resize);

    fs::remove_all(root, ec);
    return 0;
}
