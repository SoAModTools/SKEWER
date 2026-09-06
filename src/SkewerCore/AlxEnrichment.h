#pragma once

#include "Diagnostics.h"

#include "SPICE/SpiceEct/EctModel.h"
#include "SPICE/SpiceTrade/AlxModel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace skewer::core {

enum class FormationResolutionStatus { Unique, Missing, Ambiguous };
enum class EnemyJoinStatus { Unique, Missing, Ambiguous, Empty };

struct AlxLocalizedName {
    std::string japanese{};
    std::optional<std::string> localized{};
};

struct AlxEnemyRecord {
    std::uint32_t entryId = 0U;
    AlxLocalizedName name{};
};

struct AlxEnemyReference {
    std::uint8_t enemyId = 255U;
    AlxLocalizedName name{};
};

struct AlxFormationRecord {
    std::string filter{};
    std::uint32_t entryId = 0U;
    std::uint8_t initiative = 0U;
    std::uint8_t magicExperience = 0U;
    std::array<AlxEnemyReference, 8U> enemies{};
};

struct EnemySlotContext {
    std::size_t slotIndex = 0U;
    std::uint8_t enemyId = 255U;
    EnemyJoinStatus joinStatus = EnemyJoinStatus::Empty;
    AlxLocalizedName referenceName{};
    std::optional<AlxLocalizedName> canonicalName{};
    std::string displayName{};

    [[nodiscard]] bool empty() const noexcept { return joinStatus == EnemyJoinStatus::Empty; }
};

struct FormationResolution {
    FormationResolutionStatus status = FormationResolutionStatus::Missing;
    std::string filter{};
    std::uint16_t encounterId = 0U;
    std::optional<std::uint8_t> initiative{};
    std::optional<std::uint8_t> magicExperience{};
    std::array<EnemySlotContext, 8U> enemies{};
};

class AlxDataset final {
public:
    [[nodiscard]] static AlxDataset fromRecords(
        std::filesystem::path sourceRoot,
        spice::trade::alx::AlxLocale locale,
        std::vector<AlxEnemyRecord> enemies,
        std::vector<AlxFormationRecord> encounters);

    [[nodiscard]] const std::filesystem::path& sourceRoot() const noexcept;
    [[nodiscard]] spice::trade::alx::AlxLocale locale() const noexcept;
    [[nodiscard]] bool appearsGameCube() const noexcept;
    [[nodiscard]] FormationResolution resolveFormation(
        const std::string& fieldStem, std::uint16_t encounterId) const;
    [[nodiscard]] std::vector<Diagnostic> validateField(
        const std::string& fieldStem,
        const spice::ect::EctFlatContent& ect,
        const std::filesystem::path& ectPath = {}) const;

private:
    std::filesystem::path sourceRoot_{};
    spice::trade::alx::AlxLocale locale_{};
    std::vector<AlxEnemyRecord> enemies_{};
    std::vector<AlxFormationRecord> encounters_{};
    std::map<std::string, std::vector<std::size_t>> formationGroups_{};
    std::map<std::uint32_t, std::vector<std::size_t>> canonicalEnemies_{};
    bool appearsGameCube_ = false;
};

struct AlxLoadResult {
    std::optional<AlxDataset> dataset{};
    std::vector<Diagnostic> diagnostics{};

    [[nodiscard]] bool ok() const noexcept { return dataset.has_value() && !hasErrors(diagnostics); }
};

[[nodiscard]] std::string expectedDreamcastFormationFilter(const std::string& fieldStem);
[[nodiscard]] AlxLoadResult loadAlxDataset(const std::filesystem::path& sourceRoot);

} // namespace skewer::core
