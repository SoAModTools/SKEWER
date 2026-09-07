#include "SkewerCore/FieldDocument.h"
#include "SkewerCore/FieldPatch.h"
#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>

namespace {

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<unsigned long long> sequence{ 0 };
        path = std::filesystem::temp_directory_path() /
            ("skewer-authoring-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(sequence++));
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() { std::error_code error{}; std::filesystem::remove_all(path, error); }
    std::filesystem::path path{};
};

skewer::core::FieldDocument documentWithTwoTriangles(std::uint8_t firstSelector = 1) {
    skewer::core::FieldDocument document{};
    document.assets = { "a001a", "a001a.ect", "a001a.mld" };
    document.mld.sourcePlatform =
        spice::mld::model::TargetPlatform::Dreamcast;
    document.scene.triangles = {
        { skewer::core::GrndTriangleKey{ 0x1000U, 0U }, 0U, {}, { 0U, 0U, static_cast<std::uint16_t>(firstSelector * 10U) }, firstSelector },
        { skewer::core::GobjTriangleKey{ 0x2000U, 2U, 0U }, 0U, {}, { 0U, 0U, 20U }, 2U },
    };
    spice::ect::EctFlatContent flat{};
    flat.tables.resize(8U);
    flat.tables[0].stage = 1U;
    flat.tables[1].stage = 2U;
    document.ect.content = flat;
    document.workingEct = document.ect;
    return document;
}

} // namespace

TEST(FieldDocumentEditing, BulkSelectorAssignmentIsOneUndoableTransactionAndNineIsRejected) {
    auto document = documentWithTwoTriangles();
    const std::array<skewer::core::TriangleKey, 2> keys{
        document.scene.triangles[0].key, document.scene.triangles[1].key
    };
    const auto changed = document.setTriangleSelectors(keys, 6U);
    ASSERT_TRUE(changed.ok());
    ASSERT_TRUE(changed.changed);
    EXPECT_EQ(changed.changes.triangleSelectorKeys.size(), 2U);
    EXPECT_TRUE(changed.changes.ectValueKeys.empty());
    EXPECT_EQ(document.selectorEdits().size(), 2U);
    EXPECT_EQ(document.scene.triangles[0].selector, 6U);
    const auto undone = document.undo();
    EXPECT_TRUE(undone.changed);
    EXPECT_EQ(undone.changes.triangleSelectorKeys.size(), 2U);
    EXPECT_EQ(document.scene.triangles[0].selector, 1U);
    EXPECT_EQ(document.scene.triangles[1].selector, 2U);
    EXPECT_FALSE(document.isDirty());
    const auto redone = document.redo();
    EXPECT_TRUE(redone.changed);
    EXPECT_EQ(redone.changes.triangleSelectorKeys.size(), 2U);
    EXPECT_EQ(document.selectorEdits().size(), 2U);

    const auto malformed = document.setTriangleSelectors(keys, 9U);
    EXPECT_FALSE(malformed.ok());
    EXPECT_FALSE(malformed.changed);
    EXPECT_TRUE(malformed.changes.empty());
    EXPECT_EQ(document.scene.triangles[0].selector, 6U);
}

TEST(FieldDocumentEditing, EctEditsUseFullUint16RangeAndReturnToBaselineSparsely) {
    auto document = documentWithTwoTriangles();
    const skewer::core::EctValueKey stage{ skewer::core::EctValueKind::Stage, 3U, 0U };
    const auto changed = document.setEctValue(stage, 65535U);
    ASSERT_TRUE(changed.changed);
    ASSERT_EQ(changed.changes.ectValueKeys.size(), 1U);
    EXPECT_EQ(changed.changes.ectValueKeys.front(), stage);
    EXPECT_TRUE(changed.diagnostics.empty());
    EXPECT_EQ(document.effectiveEctValue(stage), 65535U);
    EXPECT_TRUE(document.isEctValueModified(stage));
    ASSERT_TRUE(document.setEctValue(stage, 0U).changed);
    EXPECT_FALSE(document.isEctValueModified(stage));
}

TEST(FieldDocumentEditing, NoOpsAndRejectedKeysHaveNoSemanticChanges) {
    auto document = documentWithTwoTriangles();
    const std::array<skewer::core::TriangleKey, 1> triangle{
        document.scene.triangles.front().key
    };
    const skewer::core::EctValueKey stage{
        skewer::core::EctValueKind::Stage, 3U, 0U
    };

    const auto selectorNoOp = document.setTriangleSelectors(triangle, 1U);
    EXPECT_TRUE(selectorNoOp.ok());
    EXPECT_FALSE(selectorNoOp.changed);
    EXPECT_TRUE(selectorNoOp.changes.empty());

    const auto ectNoOp = document.setEctValue(stage, 0U);
    EXPECT_TRUE(ectNoOp.ok());
    EXPECT_FALSE(ectNoOp.changed);
    EXPECT_TRUE(ectNoOp.changes.empty());

    const auto rejectedSelector = document.setTriangleSelectors(triangle, 9U);
    EXPECT_FALSE(rejectedSelector.ok());
    EXPECT_FALSE(rejectedSelector.changed);
    EXPECT_TRUE(rejectedSelector.changes.empty());

    const auto rejectedEct = document.setEctValue(
        { skewer::core::EctValueKind::Weight, 99U, 0U }, 25U);
    EXPECT_FALSE(rejectedEct.ok());
    EXPECT_FALSE(rejectedEct.changed);
    EXPECT_TRUE(rejectedEct.changes.empty());
}

TEST(FieldDocumentEditing, RestoreUndoAndRedoReturnExactMixedChanges) {
    auto document = documentWithTwoTriangles();
    const auto triangle = document.scene.triangles.front().key;
    const std::array<skewer::core::TriangleKey, 1> triangles{ triangle };
    const skewer::core::EctValueKey weight{
        skewer::core::EctValueKind::Weight, 0U, 0U
    };
    ASSERT_TRUE(document.setTriangleSelectors(triangles, 6U).changed);
    ASSERT_TRUE(document.setEctValue(weight, 25U).changed);

    const auto restored = document.restoreAll();
    ASSERT_TRUE(restored.changed);
    ASSERT_EQ(restored.changes.triangleSelectorKeys.size(), 1U);
    EXPECT_EQ(restored.changes.triangleSelectorKeys.front(), triangle);
    ASSERT_EQ(restored.changes.ectValueKeys.size(), 1U);
    EXPECT_EQ(restored.changes.ectValueKeys.front(), weight);
    EXPECT_FALSE(document.isDirty());

    const auto undone = document.undo();
    ASSERT_TRUE(undone.changed);
    EXPECT_EQ(undone.changes.triangleSelectorKeys,
        restored.changes.triangleSelectorKeys);
    EXPECT_EQ(undone.changes.ectValueKeys, restored.changes.ectValueKeys);
    EXPECT_TRUE(document.isDirty());

    const auto redone = document.redo();
    ASSERT_TRUE(redone.changed);
    EXPECT_EQ(redone.changes.triangleSelectorKeys,
        restored.changes.triangleSelectorKeys);
    EXPECT_EQ(redone.changes.ectValueKeys, restored.changes.ectValueKeys);
    EXPECT_FALSE(document.isDirty());
}

TEST(FieldDocumentEditing, SelectorIndexUpdatesEveryRepeatedSemanticInstance) {
    auto document = documentWithTwoTriangles();
    document.scene.triangles.push_back(document.scene.triangles.front());
    document.scene.triangles.back().batchIndex = 4U;
    const std::array<skewer::core::TriangleKey, 1> key{
        document.scene.triangles.front().key
    };

    const auto changed = document.setTriangleSelectors(key, 7U);

    ASSERT_TRUE(changed.changed);
    EXPECT_EQ(document.scene.triangles.front().selector, 7U);
    EXPECT_EQ(document.scene.triangles.back().selector, 7U);
    ASSERT_TRUE(document.undo().changed);
    EXPECT_EQ(document.scene.triangles.front().selector, 1U);
    EXPECT_EQ(document.scene.triangles.back().selector, 1U);
}

TEST(FieldDocumentEditing, ChangeSetsMergeMixedRebaseResultsWithoutDuplicates) {
    auto document = documentWithTwoTriangles();
    const std::array<skewer::core::TriangleKey, 1> triangle{
        document.scene.triangles.front().key
    };
    const skewer::core::EctValueKey weight{
        skewer::core::EctValueKind::Weight, 0U, 0U
    };
    auto changes = document.setTriangleSelectors(triangle, 5U).changes;
    const auto ectChanges = document.setEctValue(weight, 100U).changes;

    changes.merge(ectChanges);
    changes.merge(ectChanges);

    EXPECT_EQ(changes.triangleSelectorKeys.size(), 1U);
    ASSERT_EQ(changes.ectValueKeys.size(), 1U);
    EXPECT_EQ(changes.ectValueKeys.front(), weight);
}

TEST(FieldDocumentEditing, EctValidationRepresentsCurrentStateWithoutHistory) {
    auto document = documentWithTwoTriangles();
    auto* flat = std::get_if<spice::ect::EctFlatContent>(&document.workingEct.content);
    ASSERT_NE(flat, nullptr);
    for (auto& table : flat->tables) table.encounters[0].encounterRate = 100U;
    document.ect = document.workingEct;
    const skewer::core::EctValueKey weight{
        skewer::core::EctValueKind::Weight, 0U, 0U
    };

    ASSERT_TRUE(document.setEctValue(weight, 90U).changed);
    const auto first = document.validateWorkingEct();
    const auto second = document.validateWorkingEct();
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(second, first);

    ASSERT_TRUE(document.setEctValue(weight, 100U).changed);
    EXPECT_TRUE(document.validateWorkingEct().empty());
}

TEST(FieldDocumentEditing, StageZeroIsAnErrorOnlyForUsedEncounterSelectors) {
    auto document = documentWithTwoTriangles();
    auto* flat = std::get_if<spice::ect::EctFlatContent>(
        &document.workingEct.content);
    ASSERT_NE(flat, nullptr);
    for (auto& table : flat->tables) {
        table.encounters[0].encounterRate = 100U;
    }
    flat->tables[0].stage = 0U;
    document.scene.triangles.push_back(document.scene.triangles.front());

    const auto usedStageZero = document.validateWorkingEct();
    const auto errorCount = std::count_if(
        usedStageZero.begin(), usedStageZero.end(), [](const auto& diagnostic) {
            return diagnostic.severity ==
                skewer::core::DiagnosticSeverity::Error;
        });
    EXPECT_EQ(errorCount, 1U);
    EXPECT_TRUE(std::any_of(
        usedStageZero.begin(), usedStageZero.end(), [](const auto& diagnostic) {
            return diagnostic.message.find(
                "selector 1") != std::string::npos &&
                diagnostic.message.find(
                    "table 1") != std::string::npos;
        }));

    document.mld.sourcePlatform =
        spice::mld::model::TargetPlatform::GameCube;
    EXPECT_FALSE(skewer::core::hasErrors(document.validateWorkingEct()));
    document.mld.sourcePlatform =
        spice::mld::model::TargetPlatform::Dreamcast;

    for (auto& triangle : document.scene.triangles) triangle.selector = 0U;
    flat->tables[1].stage = 0U;
    EXPECT_FALSE(skewer::core::hasErrors(document.validateWorkingEct()));
}

TEST(FieldDocumentEditing, SelectorStageValidationTracksUndoRedoAndRestore) {
    auto document = documentWithTwoTriangles();
    auto* flat = std::get_if<spice::ect::EctFlatContent>(
        &document.workingEct.content);
    ASSERT_NE(flat, nullptr);
    for (auto& table : flat->tables) {
        table.encounters[0].encounterRate = 100U;
    }
    document.ect = document.workingEct;
    const std::array<skewer::core::TriangleKey, 1> triangle{
        document.scene.triangles.front().key
    };
    const skewer::core::EctValueKey tableEightStage{
        skewer::core::EctValueKind::Stage, 7U, 0U
    };

    ASSERT_TRUE(document.setTriangleSelectors(triangle, 8U).changed);
    EXPECT_TRUE(skewer::core::hasErrors(document.validateWorkingEct()));
    ASSERT_TRUE(document.setEctValue(tableEightStage, 2U).changed);
    EXPECT_FALSE(skewer::core::hasErrors(document.validateWorkingEct()));

    ASSERT_TRUE(document.undo().changed);
    EXPECT_TRUE(skewer::core::hasErrors(document.validateWorkingEct()));
    ASSERT_TRUE(document.undo().changed);
    EXPECT_FALSE(skewer::core::hasErrors(document.validateWorkingEct()));
    ASSERT_TRUE(document.redo().changed);
    EXPECT_TRUE(skewer::core::hasErrors(document.validateWorkingEct()));
    ASSERT_TRUE(document.redo().changed);
    EXPECT_FALSE(skewer::core::hasErrors(document.validateWorkingEct()));

    const auto restored = document.restoreAll();
    ASSERT_TRUE(restored.changed);
    EXPECT_FALSE(skewer::core::hasErrors(document.validateWorkingEct()));
    EXPECT_EQ(restored.changes.triangleSelectorKeys.size(), 1U);
    EXPECT_EQ(restored.changes.ectValueKeys.size(), 1U);
}

TEST(FieldPatch, DeterministicRoundTripAndConflictPreservation) {
    auto document = documentWithTwoTriangles();
    const std::array<skewer::core::TriangleKey, 1> key{ document.scene.triangles[0].key };
    ASSERT_TRUE(document.setTriangleSelectors(key, 4U).changed);
    ASSERT_TRUE(document.setEctValue({ skewer::core::EctValueKind::Weight, 1U, 7U }, 25U).changed);
    const auto patch = skewer::core::makeFieldPatch(document);
    const auto json = skewer::core::serializeFieldPatch(patch);
    const auto parsed = skewer::core::parseFieldPatch(json);
    ASSERT_TRUE(parsed.ok()) << (parsed.diagnostics.empty() ? "" : parsed.diagnostics.front().message);
    EXPECT_EQ(*parsed.patch, patch);
    EXPECT_EQ(skewer::core::serializeFieldPatch(*parsed.patch), json);

    auto changedSource = documentWithTwoTriangles(3U);
    const auto restored = skewer::core::restoreFieldPatch(changedSource, patch);
    ASSERT_EQ(restored.conflicts.size(), 1U);
    EXPECT_EQ(changedSource.scene.triangles[0].selector, 3U);
    EXPECT_EQ(restored.preservedTriangleEdits.size(), 1U);
    EXPECT_EQ(changedSource.effectiveEctValue({ skewer::core::EctValueKind::Weight, 1U, 7U }), 25U);
}

TEST(FieldPatch, SelectorNineIsNotSchemaRepresentable) {
    auto document = documentWithTwoTriangles();
    skewer::core::FieldPatch patch{ "a001a", "a001a.ect", "a001a.mld" };
    patch.triangleSelectorEdits.push_back({ document.scene.triangles[0].key, 1U, 9U });
    EXPECT_TRUE(skewer::core::hasErrors(skewer::core::validateFieldPatch(patch)));
    const auto malformed = skewer::core::parseFieldPatch(R"({
      "format":"skewer-field-patch","version":1,
      "field":{"stem":"a001a","platform":"dreamcast","ectFile":"a001a.ect","mldFile":"a001a.mld"},
      "mld":{"triangleSelectorEdits":[{"key":{"kind":"grnd","resourceAddress":"0x00001000","triangleIndex":0},"expectedSelector":1,"selector":9}]},
      "ect":{"valueEdits":[]}
    })");
    EXPECT_FALSE(malformed.ok());
}

TEST(FieldPatchStore, SavesLoadsAndRemovesCanonicalPatch) {
    TempDirectory temp{};
    skewer::core::FieldPatchStore store(temp.path);
    auto document = documentWithTwoTriangles();
    const std::array<skewer::core::TriangleKey, 1> key{ document.scene.triangles[0].key };
    ASSERT_TRUE(document.setTriangleSelectors(key, 5U).changed);
    const auto patch = skewer::core::makeFieldPatch(document);
    std::vector<skewer::core::Diagnostic> diagnostics{};
    ASSERT_TRUE(store.save(patch, diagnostics));
    ASSERT_TRUE(store.load("a001a").ok());
    const auto stems = store.listPatchStems(diagnostics);
    ASSERT_EQ(stems, std::vector<std::string>{ "a001a" });
    ASSERT_TRUE(store.remove("a001a", diagnostics));
    EXPECT_FALSE(std::filesystem::exists(store.patchPath("a001a")));
}
