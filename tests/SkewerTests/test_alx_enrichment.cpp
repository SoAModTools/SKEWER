#include "RealCorpus.h"

#include "SkewerCore/AlxEnrichment.h"
#include "SkewerCore/FieldLoader.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>

namespace {

using spice::trade::alx::AlxLocale;
using skewer::core::AlxEnemyRecord;
using skewer::core::AlxFormationRecord;
using skewer::core::AlxLocalizedName;

[[nodiscard]] AlxLocalizedName name(std::string japanese, std::string localized) {
    return { std::move(japanese), std::move(localized) };
}

[[nodiscard]] AlxEnemyRecord enemy(const std::uint32_t id, std::string localized) {
    AlxEnemyRecord result{};
    result.entryId = id;
    result.name = name("JP " + localized, std::move(localized));
    return result;
}

[[nodiscard]] AlxFormationRecord formation(const std::string& filter,
    const std::uint32_t id, const std::uint8_t enemyId, const std::string& localized) {
    AlxFormationRecord result{};
    result.filter = filter;
    result.entryId = id;
    result.initiative = 52U;
    result.magicExperience = 3U;
    for (auto& slot : result.enemies) {
        slot.enemyId = 255U;
        slot.name = name("None", "None");
    }
    result.enemies[0].enemyId = enemyId;
    result.enemies[0].name = name("JP " + localized, localized);
    return result;
}

[[nodiscard]] bool hasMessage(const std::vector<skewer::core::Diagnostic>& diagnostics,
    const std::string_view text) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [text](const auto& diagnostic) {
        return diagnostic.message.find(text) != std::string::npos;
    });
}

[[nodiscard]] std::filesystem::path repositoryRoot() {
    auto candidate = std::filesystem::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        if (std::filesystem::exists(candidate / "SKEWER.sln") &&
            std::filesystem::exists(candidate / "SPICE/SpiceTrade")) return candidate;
        if (!candidate.has_parent_path()) break;
        candidate = candidate.parent_path();
    }
    return {};
}

[[nodiscard]] std::filesystem::path privateAlxCorpusRoot() {
    const auto root = repositoryRoot();
    return root.empty()
        ? std::filesystem::path{}
        : root / "SPICE/SpiceTrade/Alx v5.0.0 corpuses";
}

} // namespace

TEST(AlxEnrichment, UsesDreamcastFilterConvention) {
    EXPECT_EQ(skewer::core::expectedDreamcastFormationFilter("a106a"), "A106A_EP.BIN");
}

TEST(AlxEnrichment, RequiresBothCanonicalCsvFilesInTheSelectedDirectory) {
    const auto root = repositoryRoot();
    if (root.empty()) GTEST_SKIP() << "Repository root is unavailable.";
    const auto loaded = skewer::core::loadAlxDataset(root / "tmp/alx-directory-that-does-not-exist");
    EXPECT_FALSE(loaded.ok());
    EXPECT_FALSE(loaded.dataset.has_value());
    EXPECT_TRUE(hasMessage(loaded.diagnostics, "is missing"));
}

TEST(AlxEnrichment, ResolvesWithinFieldAndJoinsCanonicalEnemyById) {
    std::vector<AlxEnemyRecord> enemies{ enemy(7U, "Canonical") };
    std::vector<AlxFormationRecord> encounters{
        formation("A106A_EP.BIN", 4U, 7U, "Canonical"),
        formation("A109B_EP.BIN", 4U, 8U, "Other field"),
    };
    const auto dataset = skewer::core::AlxDataset::fromRecords(
        "alx", AlxLocale::UnitedStates, std::move(enemies), std::move(encounters));

    const auto resolved = dataset.resolveFormation("a106a", 4U);
    ASSERT_EQ(resolved.status, skewer::core::FormationResolutionStatus::Unique);
    EXPECT_EQ(resolved.filter, "A106A_EP.BIN");
    EXPECT_EQ(resolved.initiative, 52U);
    EXPECT_EQ(resolved.magicExperience, 3U);
    EXPECT_EQ(resolved.enemies[0].joinStatus, skewer::core::EnemyJoinStatus::Unique);
    EXPECT_EQ(resolved.enemies[0].displayName, "Canonical");
    EXPECT_TRUE(resolved.enemies[1].empty());
}

TEST(AlxEnrichment, PreservesMissingAndAmbiguousJoinsWithoutGuessing) {
    std::vector<AlxEnemyRecord> enemies{ enemy(7U, "First"), enemy(7U, "Second") };
    std::vector<AlxFormationRecord> encounters{
        formation("a106a_ep.bin", 4U, 7U, "Reference"),
        formation("A106A_EP.BIN", 5U, 9U, "Fallback"),
        formation("A106A_EP.BIN", 6U, 9U, "Duplicate one"),
        formation("A106A_EP.BIN", 6U, 9U, "Duplicate two"),
    };
    const auto dataset = skewer::core::AlxDataset::fromRecords(
        "alx", AlxLocale::UnitedStates, std::move(enemies), std::move(encounters));

    const auto ambiguousEnemy = dataset.resolveFormation("A106A", 4U);
    ASSERT_EQ(ambiguousEnemy.status, skewer::core::FormationResolutionStatus::Unique);
    EXPECT_EQ(ambiguousEnemy.enemies[0].joinStatus, skewer::core::EnemyJoinStatus::Ambiguous);
    EXPECT_EQ(ambiguousEnemy.enemies[0].displayName, "Reference");
    const auto missingEnemy = dataset.resolveFormation("A106A", 5U);
    EXPECT_EQ(missingEnemy.enemies[0].joinStatus, skewer::core::EnemyJoinStatus::Missing);
    EXPECT_EQ(missingEnemy.enemies[0].displayName, "Fallback");
    EXPECT_EQ(dataset.resolveFormation("A106A", 6U).status,
        skewer::core::FormationResolutionStatus::Ambiguous);
}

TEST(AlxEnrichment, FallsBackToJapaneseCanonicalName) {
    AlxEnemyRecord canonical{};
    canonical.entryId = 7U;
    canonical.name = { "カノニカル", std::nullopt };
    std::vector<AlxEnemyRecord> enemies{ canonical };
    auto encounter = formation("A106A_EP.BIN", 4U, 7U, "Reference");
    encounter.enemies[0].name = { "カノニカル", std::nullopt };
    std::vector<AlxFormationRecord> encounters{ encounter };
    const auto dataset = skewer::core::AlxDataset::fromRecords(
        "alx", AlxLocale::Japanese, std::move(enemies), std::move(encounters));

    const auto resolved = dataset.resolveFormation("A106A", 4U);
    ASSERT_EQ(resolved.status, skewer::core::FormationResolutionStatus::Unique);
    EXPECT_EQ(resolved.enemies[0].displayName, "カノニカル");
}

TEST(AlxEnrichment, ValidatesOnlyEncounterIdsWithNonzeroWeight) {
    std::vector<AlxEnemyRecord> enemies{ enemy(7U, "Canonical") };
    std::vector<AlxFormationRecord> encounters{
        formation("A106A_EP.BIN", 4U, 7U, "Canonical"),
    };
    const auto dataset = skewer::core::AlxDataset::fromRecords(
        "alx", AlxLocale::UnitedStates, std::move(enemies), std::move(encounters));
    spice::ect::EctFlatContent ect{};
    ect.tables.resize(1U);
    ect.tables[0].encounters[0].encounterId = 99U;

    EXPECT_TRUE(dataset.validateField("A106A", ect).empty());
    ect.tables[0].encounters[0].encounterRate = 1U;
    const auto diagnostics = dataset.validateField("A106A", ect);
    EXPECT_TRUE(hasMessage(diagnostics, "encounter ID 99"));
}

TEST(AlxEnrichment, DiagnosesCanonicalDuplicatesAndNameDisagreement) {
    std::vector<AlxEnemyRecord> enemies{
        enemy(7U, "Canonical"), enemy(8U, "First"), enemy(8U, "Second"),
    };
    std::vector<AlxFormationRecord> encounters{
        formation("A106A_EP.BIN", 4U, 7U, "Different"),
        formation("A106A_EP.BIN", 5U, 8U, "Reference"),
    };
    const auto dataset = skewer::core::AlxDataset::fromRecords(
        "alx", AlxLocale::UnitedStates, std::move(enemies), std::move(encounters));
    spice::ect::EctFlatContent ect{};
    ect.tables.resize(1U);
    ect.tables[0].encounters[0] = { 4U, 1U };
    ect.tables[0].encounters[1] = { 5U, 1U };

    const auto diagnostics = dataset.validateField("A106A", ect);
    EXPECT_TRUE(hasMessage(diagnostics, "name disagrees"));
    EXPECT_TRUE(hasMessage(diagnostics, "more than one canonical"));
}

TEST(AlxEnrichment, LoadsPrivateDreamcastLocalesAndOrdinaryGroupsWhenAvailable) {
    const auto corpora = privateAlxCorpusRoot();
    if (!std::filesystem::is_directory(corpora)) {
        GTEST_SKIP() << "Private ALX 5.0.0 corpus is unavailable.";
    }
    for (const auto* profile : {
        "2000-08-28-dc-jp-final", "2000-09-18-dc-us-final", "2001-02-19-dc-eu-final" }) {
        const auto loaded = skewer::core::loadAlxDataset(corpora / profile / "disc-1");
        ASSERT_TRUE(loaded.ok()) << profile;
        EXPECT_FALSE(loaded.dataset->appearsGameCube());
        for (const auto* stem : skewer::tests::kOrdinaryDreamcastFieldStems) {
            EXPECT_EQ(loaded.dataset->resolveFormation(stem, 0U).status,
                skewer::core::FormationResolutionStatus::Unique) << profile << ' ' << stem;
        }
    }
}

TEST(AlxEnrichment, AcceptsGameCubeCorpusWithNonblockingWarning) {
    const auto corpora = privateAlxCorpusRoot();
    if (!std::filesystem::is_directory(corpora)) {
        GTEST_SKIP() << "Private ALX 5.0.0 corpus is unavailable.";
    }
    const auto loaded = skewer::core::loadAlxDataset(
        corpora / "2002-12-19-gc-us-final");
    ASSERT_TRUE(loaded.ok());
    EXPECT_TRUE(loaded.dataset->appearsGameCube());
    EXPECT_TRUE(hasMessage(loaded.diagnostics, "GameCube"));
    EXPECT_EQ(loaded.dataset->resolveFormation("A106A", 0U).status,
        skewer::core::FormationResolutionStatus::Missing);
}

TEST(AlxEnrichment, ValidatesExtendedDreamcastFieldCorpusWhenAvailable) {
    const std::filesystem::path fieldRoot = LR"(D:\SoADC\SoA(Usa)Disc1Assets\FIELD)";
    if (!std::filesystem::exists(fieldRoot)) GTEST_SKIP() << "Dreamcast FIELD corpus is unavailable.";
    const auto corpora = privateAlxCorpusRoot();
    if (!std::filesystem::is_directory(corpora)) {
        GTEST_SKIP() << "Private ALX 5.0.0 corpus is unavailable.";
    }
    const auto loaded = skewer::core::loadAlxDataset(
        corpora / "2000-09-18-dc-us-final/disc-1");
    ASSERT_TRUE(loaded.ok());
    for (const auto* stem : skewer::tests::kOrdinaryDreamcastFieldStems) {
        const auto field = skewer::core::FieldLoader::load({ stem,
            fieldRoot / (std::string(stem) + ".ECT"), fieldRoot / (std::string(stem) + ".MLD") });
        ASSERT_TRUE(field.ok()) << stem;
        const auto* flat = std::get_if<spice::ect::EctFlatContent>(&field.document->workingEct.content);
        ASSERT_NE(flat, nullptr) << stem;
        EXPECT_TRUE(loaded.dataset->validateField(stem, *flat, field.document->assets.ectPath).empty()) << stem;
    }
}
