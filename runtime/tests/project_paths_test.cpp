#include "project_paths.h"
#include <cassert>
#include <fstream>

int main() {
    namespace fs = std::filesystem;
    using namespace RuntimeProjectPaths;
    const auto root = fs::temp_directory_path() / ("wiicompiled-paths-" + std::to_string(RuntimePlatform::CurrentProcessId()));
    fs::create_directories(root / "Install" / "Base");
    std::ofstream(root / "portable.txt") << "portable\n";
    assert(FindPortableRoot(root / "Install" / "Base") == root);
    assert(!FindPortableRoot(root / "Install" / "Base", 1));
    assert(UserDataDirectory(root, "") == root / "UserData");
    assert(UserDataDirectory(root, "title-pal") == root / "UserData" / "Projects" / "title-pal");
    assert(UserDataDirectory(root, "title-pal") != UserDataDirectory(root, "title-us"));
    assert(UserDataDirectory(std::nullopt, "title-pal") ==
           RuntimePlatform::ApplicationDataDirectory("WiiCompiled") / "Projects" / "title-pal");
    for (auto invalid : {"../escape", "a/b", "a\\b", "A", "a:", "con", "nul", "com1", "lpt9", "-title"}) {
        assert(!IsValidNamespace(invalid));
        bool rejected = false;
        try { ProjectDirectory(root, invalid); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    assert(IsValidNamespace("title-rev_2"));
    fs::remove(root / "portable.txt");
    fs::remove(root / "Install" / "Base");
    fs::remove(root / "Install");
    fs::remove(root);
}
