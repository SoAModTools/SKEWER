#include "FieldSessionController.h"

#include <QDir>
#include <QtConcurrent/QtConcurrentRun>

#include "SPICE/SpiceEct/EctModel.h"
#include "SPICE/SpiceTrade/AlxModel.h"

#include <algorithm>
#include <filesystem>
#include <utility>
#include <variant>

namespace skewer::qt {

FieldSessionController::FieldSessionController(QObject* parent)
    : QObject(parent) {
    alxValidationTimer_.setSingleShot(true);
    alxValidationTimer_.setInterval(150);
    connect(&discoveryWatcher_, &QFutureWatcherBase::finished,
        this, &FieldSessionController::onDiscoveryFinished);
    connect(&fieldLoadWatcher_, &QFutureWatcherBase::finished,
        this, &FieldSessionController::onFieldLoadFinished);
    connect(&alxLoadWatcher_, &QFutureWatcherBase::finished,
        this, &FieldSessionController::onAlxLoadFinished);
    connect(&alxValidationTimer_, &QTimer::timeout,
        this, &FieldSessionController::startAlxValidation);
    connect(&alxValidationWatcher_, &QFutureWatcherBase::finished,
        this, &FieldSessionController::onAlxValidationFinished);
}

bool FieldSessionController::beginDiscovery(
    const QString& rootPath, QString restoreField) {
    if (rootPath.isEmpty() || discoveryRunning() || fieldLoadRunning()) return false;
    currentRoot_ = QDir::cleanPath(rootPath);
    pendingRestoreField_ = std::move(restoreField);
    document_.reset();
    requestActiveFieldAlxValidation();
    emit documentCleared();
    emit discoveryStarted();
    const auto path = std::filesystem::path(currentRoot_.toStdWString());
    discoveryWatcher_.setFuture(QtConcurrent::run([path]() {
        return skewer::core::FieldDiscovery::discover(path);
    }));
    return true;
}

bool FieldSessionController::beginFieldLoad(const int catalogIndex) {
    if (catalogIndex < 0 || fieldLoadRunning()) return false;
    if (static_cast<std::size_t>(catalogIndex) >= catalog_.fields.size()) return false;
    const auto assets = catalog_.fields[static_cast<std::size_t>(catalogIndex)].assetPair();
    if (!assets.has_value()) return false;
    document_.reset();
    requestActiveFieldAlxValidation();
    emit documentCleared();
    emit fieldLoadStarted(QString::fromStdString(assets->stem));
    fieldLoadWatcher_.setFuture(QtConcurrent::run([assets = *assets]() {
        return skewer::core::FieldLoader::load(assets);
    }));
    return true;
}

bool FieldSessionController::beginAlxLoad(
    const QString& rootPath, const bool interactive) {
    if (rootPath.isEmpty() || alxLoadRunning()) return false;
    pendingAlxRoot_ = QDir::cleanPath(rootPath);
    alxLoadInteractive_ = interactive;
    emit alxLoadStarted(pendingAlxRoot_);
    const auto path = std::filesystem::path(pendingAlxRoot_.toStdWString());
    alxLoadWatcher_.setFuture(QtConcurrent::run([path]() {
        return skewer::core::loadAlxDataset(path);
    }));
    return true;
}

void FieldSessionController::clearAlxData() {
    if (alxLoadRunning()) return;
    alxDataset_.reset();
    alxDataRoot_.clear();
    pendingAlxRoot_.clear();
    alxLoadDiagnostics_.clear();
    emit alxChanged();
    requestActiveFieldAlxValidation();
}

void FieldSessionController::restoreRememberedAlxRoot(const QString& rootPath) {
    if (!rootPath.isEmpty()) alxDataRoot_ = QDir::cleanPath(rootPath);
}

void FieldSessionController::applyTriangleSelectors(
    const std::span<const skewer::core::TriangleKey> keys,
    const std::uint8_t selector) {
    if (document_ == nullptr || keys.empty()) return;
    const auto result = document_->setTriangleSelectors(keys, selector,
        keys.size() == 1U
            ? "Set triangle encounter selector"
            : "Set triangle encounter selectors");
    if (result.changed) emit semanticChangesApplied(result.changes);
    emit editDiagnosticsChanged(result.diagnostics);
}

void FieldSessionController::applyEctValue(
    const skewer::core::EctValueKey& key,
    const QString& text) {
    if (document_ == nullptr) return;
    bool valid = false;
    const auto value = text.toUInt(&valid);
    if (!valid || value > 65535U) {
        const std::vector<skewer::core::Diagnostic> diagnostics{
            { skewer::core::DiagnosticSeverity::Warning,
                "ECT values must be integers from 0 through 65535.",
                document_->assets.ectPath }
        };
        emit editDiagnosticsChanged(diagnostics);
        emit ectEditRejected(key);
        return;
    }
    const auto result = document_->setEctValue(
        key, static_cast<std::uint16_t>(value));
    if (result.changed) emit semanticChangesApplied(result.changes);
    emit editDiagnosticsChanged(result.diagnostics);
}

void FieldSessionController::undo() {
    if (document_ == nullptr) return;
    const auto result = document_->undo();
    if (result.changed) emit semanticChangesApplied(result.changes);
}

void FieldSessionController::redo() {
    if (document_ == nullptr) return;
    const auto result = document_->redo();
    if (result.changed) emit semanticChangesApplied(result.changes);
}

void FieldSessionController::requestActiveFieldAlxValidation() {
    ++requestedAlxValidationRevision_;
    alxValidationTimer_.stop();
    if (alxDataset_ == nullptr || document_ == nullptr) {
        alxValidationPending_ = false;
        emit alxValidationChanged({});
        return;
    }
    alxValidationTimer_.start();
}

bool FieldSessionController::discoveryRunning() const noexcept {
    return discoveryWatcher_.isRunning();
}

bool FieldSessionController::fieldLoadRunning() const noexcept {
    return fieldLoadWatcher_.isRunning();
}

bool FieldSessionController::alxLoadRunning() const noexcept {
    return alxLoadWatcher_.isRunning();
}

const skewer::core::FieldDiscoveryResult& FieldSessionController::catalog() const noexcept {
    return catalog_;
}

QString FieldSessionController::takePendingRestoreField() {
    return std::exchange(pendingRestoreField_, {});
}

const QString& FieldSessionController::currentRoot() const noexcept {
    return currentRoot_;
}

const QString& FieldSessionController::alxDataRoot() const noexcept {
    return alxDataRoot_;
}

const QString& FieldSessionController::pendingAlxRoot() const noexcept {
    return pendingAlxRoot_;
}

bool FieldSessionController::completedAlxLoadWasInteractive() const noexcept {
    return completedAlxLoadInteractive_;
}

const std::vector<skewer::core::Diagnostic>&
FieldSessionController::alxLoadDiagnostics() const noexcept {
    return alxLoadDiagnostics_;
}

bool FieldSessionController::hasAlxDataset() const noexcept {
    return alxDataset_ != nullptr;
}

QString FieldSessionController::alxLocaleName() const {
    if (alxDataset_ == nullptr) return {};
    return QString::fromLatin1(spice::trade::alx::toString(alxDataset_->locale()));
}

QString FieldSessionController::alxSourceRoot() const {
    if (alxDataset_ == nullptr) return {};
    return QString::fromStdWString(alxDataset_->sourceRoot().wstring());
}

skewer::core::FieldDocument* FieldSessionController::document() noexcept {
    return document_.get();
}

const skewer::core::FieldDocument* FieldSessionController::document() const noexcept {
    return document_.get();
}

std::optional<std::int32_t> FieldSessionController::groundTblIdForEntry(
    const std::size_t entryTableIndex) const {
    if (document_ == nullptr) return std::nullopt;
    const auto found = std::find_if(document_->mld.entries.begin(), document_->mld.entries.end(),
        [entryTableIndex](const auto& record) {
            return record.entry.tableIndex == entryTableIndex;
        });
    if (found == document_->mld.entries.end()) return std::nullopt;
    return found->entry.tblId;
}

std::optional<skewer::core::FormationResolution>
FieldSessionController::resolveFormation(
    const int tableIndex, const int rowIndex) const {
    if (alxDataset_ == nullptr || document_ == nullptr ||
        tableIndex < 0 || rowIndex < 0) {
        return std::nullopt;
    }
    const auto* flat = std::get_if<spice::ect::EctFlatContent>(
        &document_->workingEct.content);
    const auto table = static_cast<std::size_t>(tableIndex);
    const auto row = static_cast<std::size_t>(rowIndex);
    if (flat == nullptr || table >= flat->tables.size() ||
        row >= flat->tables[table].encounters.size()) {
        return std::nullopt;
    }
    return alxDataset_->resolveFormation(
        document_->assets.stem,
        flat->tables[table].encounters[row].encounterId);
}

std::vector<std::uint8_t> FieldSessionController::modifiedSceneBatches() const {
    if (document_ == nullptr) return {};
    std::vector<std::uint8_t> modified(document_->scene.batches.size(), 0U);
    for (std::size_t batchIndex = 0;
        batchIndex < document_->scene.batches.size(); ++batchIndex) {
        const auto& batch = document_->scene.batches[batchIndex];
        modified[batchIndex] = std::any_of(
            batch.triangleIndices.begin(), batch.triangleIndices.end(),
            [&](const std::size_t index) {
                return index < document_->scene.triangles.size() &&
                    document_->isTriangleModified(
                        document_->scene.triangles[index].key);
            }) ? 1U : 0U;
    }
    return modified;
}

void FieldSessionController::onDiscoveryFinished() {
    catalog_ = discoveryWatcher_.future().takeResult();
    emit diagnosticsProduced(catalog_.diagnostics, true);
    emit discoveryFinished(catalog_.ok());
}

void FieldSessionController::onFieldLoadFinished() {
    auto result = fieldLoadWatcher_.future().takeResult();
    emit diagnosticsProduced(result.diagnostics, true);
    if (result.ok()) {
        document_ = std::make_unique<skewer::core::FieldDocument>(
            std::move(*result.document));
    }
    emit fieldLoadFinished(result.ok());
}

void FieldSessionController::onAlxLoadFinished() {
    auto result = alxLoadWatcher_.future().takeResult();
    alxLoadDiagnostics_ = result.diagnostics;
    const auto attemptedRoot = pendingAlxRoot_;
    pendingAlxRoot_.clear();
    completedAlxLoadInteractive_ = alxLoadInteractive_;
    alxLoadInteractive_ = false;
    if (result.ok()) {
        alxDataset_ = std::make_shared<const skewer::core::AlxDataset>(
            std::move(*result.dataset));
        alxDataRoot_ = attemptedRoot;
        emit alxChanged();
        requestActiveFieldAlxValidation();
    }
    emit alxLoadFinished(result.ok());
}

void FieldSessionController::startAlxValidation() {
    if (alxValidationWatcher_.isRunning()) {
        alxValidationPending_ = true;
        return;
    }
    if (alxDataset_ == nullptr || document_ == nullptr) {
        emit alxValidationChanged({});
        return;
    }
    const auto* flat = std::get_if<spice::ect::EctFlatContent>(
        &document_->workingEct.content);
    if (flat == nullptr) {
        emit alxValidationChanged({});
        return;
    }
    runningAlxValidationRevision_ = requestedAlxValidationRevision_;
    const auto revision = runningAlxValidationRevision_;
    const auto dataset = alxDataset_;
    const auto ect = *flat;
    const auto fieldStem = document_->assets.stem;
    const auto ectPath = document_->assets.ectPath;
    alxValidationPending_ = false;
    alxValidationWatcher_.setFuture(QtConcurrent::run(
        [revision, dataset, ect, fieldStem, ectPath]() {
            return AlxValidationResult{
                revision,
                dataset->validateField(fieldStem, ect, ectPath)
            };
        }));
}

void FieldSessionController::onAlxValidationFinished() {
    const auto result = alxValidationWatcher_.future().takeResult();
    if (result.revision == requestedAlxValidationRevision_) {
        emit alxValidationChanged(result.diagnostics);
    }
    if (alxValidationPending_ || result.revision != requestedAlxValidationRevision_) {
        alxValidationPending_ = false;
        alxValidationTimer_.start(0);
    }
}

} // namespace skewer::qt
