#include "SkewerCore/FieldDiscovery.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

class TempTree final {
public:
    TempTree() {
        static std::atomic<unsigned long long> sequence{ 0 };
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
            ("skewer-tests-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
        std::filesystem::create_directories(path);
    }
    ~TempTree() {
        std::error_code error{};
        std::filesystem::remove_all(path, error);
    }
    void file(const std::filesystem::path& relative, const std::vector<unsigned char>& bytes = { 0 }) const {
        const auto destination = path / relative;
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream output(destination, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::filesystem::path path{};
};

} // namespace

TEST(FieldDiscovery, CountsSelectedFieldDirectoryAndDisablesUnpairedEntries) {
    TempTree tree{};
    const auto field = tree.path / "FiElD";
    std::filesystem::create_directories(field);
    tree.file("FiElD/A2.EcT");
    tree.file("FiElD/A2.mLd");
    tree.file("FiElD/A10.ECT");
    tree.file("FiElD/A099A.ECT");
    tree.file("FiElD/A099A.MLD");
    tree.file("FiElD/ONLY.MLD");

    const auto result = skewer::core::FieldDiscovery::discover(field);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.fields.size(), 3U);
    EXPECT_EQ(result.fields[0].stem, "a2");
    EXPECT_TRUE(result.fields[0].isAvailable());
    EXPECT_EQ(result.fields[1].stem, "a10");
    EXPECT_EQ(result.fields[1].availability, skewer::core::FieldAvailability::MissingMld);
    EXPECT_EQ(result.fields[2].stem, "a099a");
    EXPECT_EQ(result.fields[2].availability, skewer::core::FieldAvailability::Area99Deferred);
}

TEST(FieldDiscovery, RejectsZeroOrMultipleFieldDirectories) {
    TempTree empty{};
    EXPECT_FALSE(skewer::core::FieldDiscovery::discover(empty.path).ok());

    TempTree multiple{};
    std::filesystem::create_directories(multiple.path / "one/FIELD");
    std::filesystem::create_directories(multiple.path / "two/field");
    const auto result = skewer::core::FieldDiscovery::discover(multiple.path);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.fieldDirectoryCandidates.size(), 2U);
}

TEST(FieldDiscovery, RejectsGameCubeAklzEct) {
    TempTree tree{};
    std::filesystem::create_directories(tree.path / "FIELD");
    tree.file("FIELD/a001a.mld");
    tree.file("FIELD/a001a.ect", {
        'A', 'K', 'L', 'Z', '~', '?', 'Q', 'd', '=', 0xCC, 0xCC, 0xCD, 0, 0, 0, 0,
    });
    const auto result = skewer::core::FieldDiscovery::discover(tree.path);
    EXPECT_FALSE(result.ok());
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.back().message, "GameCube is not yet supported.");
}

TEST(FieldDiscovery, PairsOptionalSctByFieldStemCaseInsensitively) {
    TempTree tree{};
    tree.file("FIELD/A111C.ECT");
    tree.file("FIELD/A111C.MLD");
    tree.file("FIELD/mE111c.sCt");
    tree.file("FIELD/ME999A.SCT");

    const auto result = skewer::core::FieldDiscovery::discover(tree.path);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.fields.size(), 1U);
    ASSERT_TRUE(result.fields.front().sctPath.has_value());
    EXPECT_EQ(result.fields.front().sctPath->filename().string(), "mE111c.sCt");
    const auto pair = result.fields.front().assetPair();
    ASSERT_TRUE(pair.has_value());
    EXPECT_EQ(pair->sctPath, result.fields.front().sctPath);
}

TEST(FieldDiscovery, MissingSctDoesNotMakeFieldUnavailable) {
    TempTree tree{};
    tree.file("FIELD/A103B.ECT");
    tree.file("FIELD/A103B.MLD");

    const auto result = skewer::core::FieldDiscovery::discover(tree.path);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.fields.size(), 1U);
    EXPECT_TRUE(result.fields.front().isAvailable());
    EXPECT_FALSE(result.fields.front().sctPath.has_value());
}
