#include "AlxEnrichment.h"

#include "SPICE/SpiceTrade/AlxImporter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <sstream>
#include <utility>

namespace skewer::core {
namespace {

[[nodiscard]] std::string upperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

[[nodiscard]] bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

[[nodiscard]] std::string displayName(const AlxLocalizedName& name) {
    if (name.localized.has_value() && !name.localized->empty()) return *name.localized;
    return name.japanese;
}

[[nodiscard]] bool namesDisagree(
    const AlxLocalizedName& reference,
    const AlxLocalizedName& canonical) {
    if (!reference.japanese.empty() && !canonical.japanese.empty() &&
        reference.japanese != canonical.japanese) return true;
    return reference.localized.has_value() && canonical.localized.has_value() &&
        *reference.localized != *canonical.localized;
}

void appendUnique(std::vector<Diagnostic>& diagnostics, std::set<std::string>& messages,
    DiagnosticSeverity severity, std::string message, const std::filesystem::path& path) {
    if (messages.insert(message).second) diagnostics.push_back({ severity, std::move(message), path });
}

[[nodiscard]] DiagnosticSeverity convertSeverity(
    const spice::trade::alx::AlxDiagnosticSeverity severity) {
    switch (severity) {
    case spice::trade::alx::AlxDiagnosticSeverity::Info: return DiagnosticSeverity::Info;
    case spice::trade::alx::AlxDiagnosticSeverity::Warning: return DiagnosticSeverity::Warning;
    case spice::trade::alx::AlxDiagnosticSeverity::Error: return DiagnosticSeverity::Error;
    }
    return DiagnosticSeverity::Error;
}

[[nodiscard]] std::optional<std::string> importedCell(
    const spice::trade::alx::AlxDerivedContext& context,
    const std::string& identity,
    const std::string& column) {
    const auto* cells = context.imported(identity);
    if (cells == nullptr) return std::nullopt;
    const auto found = cells->find(column);
    if (found == cells->end() || found->second.empty()) return std::nullopt;
    return found->second;
}

[[nodiscard]] AlxLocalizedName canonicalName(
    const spice::trade::alx::EnemyRecord& enemy,
    const spice::trade::alx::AlxLocale locale,
    const spice::trade::alx::AlxDerivedContext& context) {
    AlxLocalizedName result{ .japanese = enemy.fields().japaneseName };
    if (locale != spice::trade::alx::AlxLocale::Japanese) {
        if (const auto localized = context.enemyName(enemy.id(), locale)) {
            result.localized = std::string(*localized);
        }
    }
    return result;
}

[[nodiscard]] AlxLocalizedName referenceName(
    const spice::trade::alx::EnemyEncounterRecord& encounter,
    const std::size_t slotIndex,
    const spice::trade::alx::AlxLocale locale,
    const spice::trade::alx::AlxDerivedContext& context) {
    const auto identity = spice::trade::alx::canonicalIdentity(encounter.id());
    const auto prefix = "[EC" + std::to_string(slotIndex + 1U);
    AlxLocalizedName result{};
    if (const auto japanese = importedCell(context, identity, prefix + " JP Name]")) {
        result.japanese = *japanese;
    }
    if (locale == spice::trade::alx::AlxLocale::UnitedStates) {
        result.localized = importedCell(context, identity, prefix + " US Name]");
    } else if (locale == spice::trade::alx::AlxLocale::Europe) {
        result.localized = importedCell(context, identity, prefix + " EU Name]");
    }
    return result;
}

} // namespace

std::string expectedDreamcastFormationFilter(const std::string& fieldStem) {
    return upperAscii(fieldStem) + "_EP.BIN";
}

AlxDataset AlxDataset::fromRecords(
    std::filesystem::path sourceRoot,
    const spice::trade::alx::AlxLocale locale,
    std::vector<AlxEnemyRecord> enemies,
    std::vector<AlxFormationRecord> encounters) {
    AlxDataset dataset{};
    dataset.sourceRoot_ = std::move(sourceRoot);
    dataset.locale_ = locale;
    dataset.enemies_ = std::move(enemies);
    dataset.encounters_ = std::move(encounters);
    bool hasDreamcastFilter = false;
    bool hasGameCubeFilter = false;
    for (std::size_t index = 0U; index < dataset.encounters_.size(); ++index) {
        const auto& encounter = dataset.encounters_[index];
        const auto normalized = upperAscii(encounter.filter);
        dataset.formationGroups_[normalized].push_back(index);
        hasDreamcastFilter = hasDreamcastFilter || endsWith(normalized, ".BIN");
        hasGameCubeFilter = hasGameCubeFilter || endsWith(normalized, ".ENP");
    }
    for (std::size_t index = 0U; index < dataset.enemies_.size(); ++index) {
        dataset.canonicalEnemies_[dataset.enemies_[index].entryId].push_back(index);
    }
    dataset.appearsGameCube_ = hasGameCubeFilter && !hasDreamcastFilter;
    return dataset;
}

const std::filesystem::path& AlxDataset::sourceRoot() const noexcept { return sourceRoot_; }
spice::trade::alx::AlxLocale AlxDataset::locale() const noexcept { return locale_; }
bool AlxDataset::appearsGameCube() const noexcept { return appearsGameCube_; }

FormationResolution AlxDataset::resolveFormation(
    const std::string& fieldStem, const std::uint16_t encounterId) const {
    FormationResolution result{};
    result.filter = expectedDreamcastFormationFilter(fieldStem);
    result.encounterId = encounterId;
    const auto group = formationGroups_.find(result.filter);
    if (group == formationGroups_.end()) return result;

    std::vector<const AlxFormationRecord*> matches{};
    for (const auto index : group->second) {
        const auto& record = encounters_[index];
        if (record.entryId == encounterId) matches.push_back(&record);
    }
    if (matches.empty()) return result;
    if (matches.size() > 1U) {
        result.status = FormationResolutionStatus::Ambiguous;
        return result;
    }

    result.status = FormationResolutionStatus::Unique;
    result.initiative = matches.front()->initiative;
    result.magicExperience = matches.front()->magicExperience;
    for (std::size_t slotIndex = 0U; slotIndex < matches.front()->enemies.size(); ++slotIndex) {
        const auto& reference = matches.front()->enemies[slotIndex];
        auto& slot = result.enemies[slotIndex];
        slot.slotIndex = slotIndex;
        slot.enemyId = reference.enemyId;
        slot.referenceName = reference.name;
        if (reference.enemyId == 255U) {
            slot.joinStatus = EnemyJoinStatus::Empty;
            slot.displayName = "Empty";
            continue;
        }
        const auto candidates = canonicalEnemies_.find(reference.enemyId);
        if (candidates == canonicalEnemies_.end() || candidates->second.empty()) {
            slot.joinStatus = EnemyJoinStatus::Missing;
            slot.displayName = displayName(reference.name);
            continue;
        }
        if (candidates->second.size() > 1U) {
            slot.joinStatus = EnemyJoinStatus::Ambiguous;
            slot.displayName = displayName(reference.name);
            continue;
        }
        slot.joinStatus = EnemyJoinStatus::Unique;
        slot.canonicalName = enemies_[candidates->second.front()].name;
        slot.displayName = displayName(*slot.canonicalName);
    }
    return result;
}

std::vector<Diagnostic> AlxDataset::validateField(
    const std::string& fieldStem,
    const spice::ect::EctFlatContent& ect,
    const std::filesystem::path& ectPath) const {
    std::vector<Diagnostic> diagnostics{};
    std::set<std::string> messages{};
    const auto expectedFilter = expectedDreamcastFormationFilter(fieldStem);
    const auto group = formationGroups_.find(expectedFilter);
    if (group == formationGroups_.end()) {
        appendUnique(diagnostics, messages, DiagnosticSeverity::Warning,
            "ALX has no Dreamcast formation group " + expectedFilter + " for field " + fieldStem + ".", ectPath);
        return diagnostics;
    }

    std::map<std::uint32_t, std::size_t> formationCounts{};
    for (const auto index : group->second) ++formationCounts[encounters_[index].entryId];
    for (const auto& [entryId, count] : formationCounts) {
        if (count > 1U) appendUnique(diagnostics, messages, DiagnosticSeverity::Warning,
            "ALX formation group " + expectedFilter + " contains duplicate encounter ID " +
                std::to_string(entryId) + ".", sourceRoot_ / "enemyencounter.csv");
    }

    std::set<std::uint16_t> referencedIds{};
    for (const auto& table : ect.tables) {
        for (const auto& row : table.encounters) {
            if (row.encounterRate != 0U) referencedIds.insert(row.encounterId);
        }
    }
    for (const auto encounterId : referencedIds) {
        const auto resolution = resolveFormation(fieldStem, encounterId);
        if (resolution.status == FormationResolutionStatus::Missing) {
            appendUnique(diagnostics, messages, DiagnosticSeverity::Warning,
                "ECT encounter ID " + std::to_string(encounterId) + " has no formation in " + expectedFilter + ".", ectPath);
            continue;
        }
        if (resolution.status == FormationResolutionStatus::Ambiguous) continue;
        for (const auto& slot : resolution.enemies) {
            if (slot.empty()) continue;
            if (slot.joinStatus == EnemyJoinStatus::Missing) {
                appendUnique(diagnostics, messages, DiagnosticSeverity::Warning,
                    "ALX enemy ID " + std::to_string(slot.enemyId) +
                        " has no canonical filter == * enemy row.", sourceRoot_ / "enemy.csv");
            } else if (slot.joinStatus == EnemyJoinStatus::Ambiguous) {
                appendUnique(diagnostics, messages, DiagnosticSeverity::Warning,
                    "ALX enemy ID " + std::to_string(slot.enemyId) +
                        " has more than one canonical filter == * enemy row.", sourceRoot_ / "enemy.csv");
            } else if (slot.canonicalName.has_value() && namesDisagree(slot.referenceName, *slot.canonicalName)) {
                appendUnique(diagnostics, messages, DiagnosticSeverity::Warning,
                    "Formation " + std::to_string(encounterId) + " enemy slot " +
                        std::to_string(slot.slotIndex + 1U) + " name disagrees with canonical enemy ID " +
                        std::to_string(slot.enemyId) + ".", sourceRoot_ / "enemyencounter.csv");
            }
        }
    }
    return diagnostics;
}

AlxLoadResult loadAlxDataset(const std::filesystem::path& sourceRoot) {
    AlxLoadResult result{};
    constexpr std::array requested{
        spice::trade::alx::AlxTableKind::Enemy,
        spice::trade::alx::AlxTableKind::EnemyEncounter,
    };
    auto imported = spice::trade::alx::AlxDatasetImporter{}.importDirectory(sourceRoot, requested);
    for (const auto& diagnostic : imported.diagnostics) {
        std::ostringstream message{};
        message << diagnostic.message;
        if (diagnostic.row.has_value()) message << " (row " << *diagnostic.row;
        if (diagnostic.column.has_value()) message << (diagnostic.row.has_value() ? ", " : " (") << "column " << *diagnostic.column;
        if (diagnostic.row.has_value() || diagnostic.column.has_value()) message << ')';
        result.diagnostics.push_back({ convertSeverity(diagnostic.severity), message.str(),
            diagnostic.path.empty() ? sourceRoot : diagnostic.path });
    }
    if (!imported.ok() || !imported.dataset->enemies.has_value() ||
        !imported.dataset->enemyEncounters.has_value()) return result;

    std::vector<AlxEnemyRecord> enemies{};
    enemies.reserve(imported.dataset->enemies->records().size());
    for (const auto& enemy : imported.dataset->enemies->records()) {
        enemies.push_back({ enemy.id().value,
            canonicalName(enemy, *imported.locale, imported.derivedContext) });
    }

    std::vector<AlxFormationRecord> encounters{};
    for (const auto& group : imported.dataset->enemyEncounters->groups()) {
        for (const auto& encounter : group.records()) {
            AlxFormationRecord record{};
            record.filter = group.owner();
            record.entryId = encounter.id().entry;
            record.initiative = encounter.fields().initiative;
            record.magicExperience = encounter.fields().magicExperience;
            for (std::size_t slotIndex = 0U; slotIndex < record.enemies.size(); ++slotIndex) {
                const auto& source = encounter.fields().enemies[slotIndex];
                auto& destination = record.enemies[slotIndex];
                destination.name = referenceName(
                    encounter, slotIndex, *imported.locale, imported.derivedContext);
                if (source.enemy.has_value()) {
                    destination.enemyId = static_cast<std::uint8_t>(source.enemy->value);
                }
            }
            encounters.push_back(std::move(record));
        }
    }

    result.dataset = AlxDataset::fromRecords(
        sourceRoot, *imported.locale, std::move(enemies), std::move(encounters));
    if (result.dataset->appearsGameCube()) {
        result.diagnostics.push_back({ DiagnosticSeverity::Warning,
            "The selected ALX data uses GameCube .enp formation filters; Dreamcast fields expect _EP.BIN groups.",
            sourceRoot / "enemyencounter.csv" });
    }
    return result;
}

} // namespace skewer::core
