#include "EventGround.h"

#include "SceneModel.h"

#include "SPICE/SpiceSCT/SctModel.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <exception>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <variant>

namespace skewer::core {
namespace {

constexpr std::size_t kMaximumEvaluationStates = 10'000U;

[[nodiscard]] std::string stateKey(
    const std::uint32_t instructionOffset,
    const std::vector<EventGroundState>& states) {
    std::string key = std::to_string(instructionOffset);
    for (const auto& state : states) {
        key.push_back(':');
        if (state.kind == EventGroundStateKind::Disabled) {
            key.push_back('d');
        } else {
            key.push_back('v');
            key += std::to_string(state.variantOrdinal);
        }
    }
    return key;
}

[[nodiscard]] std::string terminalKey(const std::vector<EventGroundState>& states) {
    return stateKey(0U, states);
}

[[nodiscard]] std::set<std::string> intersection(
    const std::set<std::string>& lhs,
    const std::set<std::string>& rhs) {
    std::set<std::string> result{};
    std::set_intersection(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        std::inserter(result, result.end()));
    return result;
}

[[nodiscard]] std::optional<std::int32_t> constantInteger(
    const spice::sct::SctParameter& parameter) {
    if (parameter.expression.has_value() && parameter.expression->program.has_value()) {
        const auto& operations = parameter.expression->program->operations;
        if (operations.size() != 1U) return std::nullopt;
        const auto* literal = std::get_if<spice::sct::SctScptValueOperation>(&operations.front());
        if (literal == nullptr) return std::nullopt;

        std::optional<double> value{};
        if (literal->kind == spice::sct::SctScptValueKind::FloatLiteral &&
            literal->payloadWords.size() == 1U) {
            value = static_cast<double>(std::bit_cast<float>(literal->payloadWords.front()));
        } else if (literal->kind == spice::sct::SctScptValueKind::DecimalLiteral &&
            literal->payloadWords.empty()) {
            const auto whole = static_cast<std::int16_t>((literal->encodingWord >> 8U) & 0xFFFFU);
            const auto fraction = static_cast<std::uint8_t>(literal->encodingWord & 0xFFU);
            value = static_cast<double>(whole) + static_cast<double>(fraction) / 256.0;
        }
        if (value.has_value() && std::isfinite(*value) &&
            std::floor(*value) == *value &&
            *value >= static_cast<double>(std::numeric_limits<std::int32_t>::min()) &&
            *value <= static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
            return static_cast<std::int32_t>(*value);
        }
        return std::nullopt;
    }
    if (parameter.rawWords.size() == 1U) {
        return std::bit_cast<std::int32_t>(parameter.rawWords.front());
    }
    return std::nullopt;
}

[[nodiscard]] std::string branchCondition(const spice::sct::SctInstruction& instruction) {
    if (!instruction.parameters.empty() && !instruction.parameters.front().displayValue.empty()) {
        return instruction.parameters.front().displayValue;
    }
    std::ostringstream out{};
    out << "branch @ 0x" << std::uppercase << std::hex << instruction.payloadOffset;
    return out.str();
}

[[nodiscard]] std::string switchCondition(
    const spice::sct::SctInstruction& instruction,
    const std::size_t pathIndex) {
    const auto choice = !instruction.parameters.empty() && !instruction.parameters.front().displayValue.empty()
        ? instruction.parameters.front().displayValue
        : std::string{ "switch" };
    return choice + " path " + std::to_string(pathIndex + 1U);
}

void appendDiagnosticOnce(
    EventGroundPresetBuildResult& result,
    std::set<std::string>& emitted,
    const DiagnosticSeverity severity,
    std::string message) {
    if (emitted.insert(message).second) result.diagnostics.push_back({ severity, std::move(message) });
}

struct EvaluationItem {
    std::uint32_t instructionOffset = 0;
    std::vector<EventGroundState> states{};
    std::set<std::string> predicates{};
};

struct TerminalState {
    std::vector<EventGroundState> states{};
    std::set<std::string> mustPredicates{};
};

} // namespace

EventGroundPresetBuildResult buildEventGroundPresets(
    const std::vector<EventGroundGroup>& groups,
    const spice::sct::SctParseResult& sct) {
    EventGroundPresetBuildResult result{};
    std::set<std::string> emittedDiagnostics{};

    for (const auto& section : sct.file.sections) {
        const bool hasOpcode114 = std::any_of(section.instructions.begin(), section.instructions.end(),
            [](const auto& instruction) { return instruction.opcode == 114U; });
        if (section.kind != spice::sct::SctSectionKind::Script || !hasOpcode114 || section.instructions.empty()) {
            continue;
        }

        std::map<std::uint32_t, const spice::sct::SctInstruction*> instructions{};
        for (const auto& instruction : section.instructions) {
            instructions.emplace(instruction.offset, &instruction);
        }
        const auto payloadBase = section.instructions.front().payloadOffset -
            section.instructions.front().offset;
        const auto sectionPayloadEnd = payloadBase + (section.endOffset - section.startOffset);

        std::vector<EventGroundState> defaults(groups.size(), EventGroundState::variant(0U));
        std::deque<EvaluationItem> worklist{};
        worklist.push_back({ section.instructions.front().offset, defaults, {} });
        std::map<std::string, std::set<std::string>> visited{};
        std::map<std::string, TerminalState> terminals{};
        std::size_t uniqueStates = 0U;
        bool sectionAborted = false;

        auto recordTerminal = [&](const EvaluationItem& item) {
            const auto key = terminalKey(item.states);
            const auto found = terminals.find(key);
            if (found == terminals.end()) {
                terminals.emplace(key, TerminalState{ item.states, item.predicates });
            } else {
                found->second.mustPredicates = intersection(found->second.mustPredicates, item.predicates);
            }
        };

        while (!worklist.empty() && !sectionAborted) {
            auto item = std::move(worklist.front());
            worklist.pop_front();
            const auto key = stateKey(item.instructionOffset, item.states);
            const auto seen = visited.find(key);
            if (seen != visited.end()) {
                const auto narrowed = intersection(seen->second, item.predicates);
                if (narrowed == seen->second) continue;
                seen->second = narrowed;
                item.predicates = narrowed;
            } else {
                visited.emplace(key, item.predicates);
                if (++uniqueStates > kMaximumEvaluationStates) {
                    appendDiagnosticOnce(result, emittedDiagnostics, DiagnosticSeverity::Warning,
                        "SCT section '" + section.id.name +
                            "' exceeded the 10000-state event-ground evaluation bound; its presets were omitted.");
                    sectionAborted = true;
                    break;
                }
            }

            const auto instructionFound = instructions.find(item.instructionOffset);
            if (instructionFound == instructions.end()) {
                recordTerminal(item);
                continue;
            }
            const auto& instruction = *instructionFound->second;

            if (instruction.opcode == 114U) {
                if (instruction.parameters.size() < 2U) {
                    appendDiagnosticOnce(result, emittedDiagnostics, DiagnosticSeverity::Warning,
                        "SCT section '" + section.id.name + "' contains opcode 114 without two decoded operands.");
                } else {
                    const auto tblId = constantInteger(instruction.parameters[0]);
                    const auto ordinal = constantInteger(instruction.parameters[1]);
                    if (!tblId.has_value() || !ordinal.has_value()) {
                        appendDiagnosticOnce(result, emittedDiagnostics, DiagnosticSeverity::Warning,
                            "SCT section '" + section.id.name +
                                "' contains opcode 114 with a non-constant integral operand; the mutation was ignored.");
                    } else {
                        std::vector<std::size_t> matches{};
                        for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
                            if (groups[groupIndex].tblId == *tblId) matches.push_back(groupIndex);
                        }
                        if (matches.empty()) {
                            appendDiagnosticOnce(result, emittedDiagnostics, DiagnosticSeverity::Info,
                                "SCT section '" + section.id.name + "' targets tblId " +
                                    std::to_string(*tblId) +
                                    ", which is not present in the selected MLD; the mutation was ignored.");
                        } else if (matches.size() != 1U) {
                            appendDiagnosticOnce(result, emittedDiagnostics, DiagnosticSeverity::Warning,
                                "SCT section '" + section.id.name + "' targets duplicate tblId " +
                                    std::to_string(*tblId) + "; the mutation was not guessed.");
                        } else if (*ordinal == -1) {
                            item.states[matches.front()] = EventGroundState::disabled();
                        } else if (*ordinal < -1) {
                            appendDiagnosticOnce(result, emittedDiagnostics, DiagnosticSeverity::Warning,
                                "SCT section '" + section.id.name + "' uses unsupported negative event-ground ordinal " +
                                    std::to_string(*ordinal) + " for tblId " + std::to_string(*tblId) + ".");
                        } else if (static_cast<std::size_t>(*ordinal) >= groups[matches.front()].variants.size()) {
                            appendDiagnosticOnce(result, emittedDiagnostics, DiagnosticSeverity::Warning,
                                "SCT section '" + section.id.name + "' selects out-of-range event-ground ordinal " +
                                    std::to_string(*ordinal) + " for tblId " + std::to_string(*tblId) + ".");
                        } else {
                            item.states[matches.front()] = EventGroundState::variant(
                                static_cast<std::size_t>(*ordinal));
                        }
                    }
                }
            }

            if (instruction.opcode == 12U) {
                recordTerminal(item);
                continue;
            }

            std::vector<const spice::sct::SctEdge*> controlEdges{};
            for (const auto& edge : section.edges) {
                if (!edge.fromPayloadOffset.has_value() || *edge.fromPayloadOffset != instruction.payloadOffset) continue;
                if (edge.type == spice::sct::SctEdgeType::BranchTrue ||
                    edge.type == spice::sct::SctEdgeType::BranchFalse ||
                    edge.type == spice::sct::SctEdgeType::SwitchCase ||
                    edge.type == spice::sct::SctEdgeType::Jump) {
                    controlEdges.push_back(&edge);
                }
            }

            if (!controlEdges.empty()) {
                for (std::size_t edgeIndex = 0; edgeIndex < controlEdges.size(); ++edgeIndex) {
                    const auto& edge = *controlEdges[edgeIndex];
                    bool crossSection = !edge.toPayloadOffset.has_value();
                    if (const auto sectionAttribute = edge.attributes.find("target_section_index");
                        sectionAttribute != edge.attributes.end()) {
                        try {
                            crossSection = std::stoul(sectionAttribute->second) != section.id.index;
                        } catch (const std::exception&) {
                            crossSection = true;
                        }
                    } else if (edge.toPayloadOffset.has_value()) {
                        crossSection = *edge.toPayloadOffset < payloadBase ||
                            *edge.toPayloadOffset >= sectionPayloadEnd;
                    }
                    if (crossSection) {
                        appendDiagnosticOnce(result, emittedDiagnostics, DiagnosticSeverity::Warning,
                            "SCT section '" + section.id.name +
                                "' has a cross-section control-flow edge; the section-local path ends at that boundary.");
                        recordTerminal(item);
                        continue;
                    }
                    const auto target = edge.toOffset.value_or(
                        static_cast<std::uint32_t>(*edge.toPayloadOffset - payloadBase));
                    if (!instructions.contains(target)) {
                        appendDiagnosticOnce(result, emittedDiagnostics, DiagnosticSeverity::Warning,
                            "SCT section '" + section.id.name +
                                "' targets an undecoded local instruction; the section-local path ends at that boundary.");
                        recordTerminal(item);
                        continue;
                    }
                    auto next = item;
                    next.instructionOffset = target;
                    if (edge.type == spice::sct::SctEdgeType::BranchTrue) {
                        next.predicates.insert(branchCondition(instruction));
                    } else if (edge.type == spice::sct::SctEdgeType::BranchFalse) {
                        next.predicates.insert("not (" + branchCondition(instruction) + ")");
                    } else if (edge.type == spice::sct::SctEdgeType::SwitchCase) {
                        next.predicates.insert(switchCondition(instruction, edgeIndex));
                    }
                    worklist.push_back(std::move(next));
                }
                continue;
            }

            const auto next = instructions.upper_bound(instruction.offset);
            if (next == instructions.end()) {
                recordTerminal(item);
            } else {
                item.instructionOffset = next->first;
                worklist.push_back(std::move(item));
            }
        }

        if (sectionAborted || terminals.empty()) continue;

        std::set<std::string> predicatesCommonToAll{};
        bool firstTerminal = true;
        for (const auto& [_, terminal] : terminals) {
            if (firstTerminal) {
                predicatesCommonToAll = terminal.mustPredicates;
                firstTerminal = false;
            } else {
                predicatesCommonToAll = intersection(predicatesCommonToAll, terminal.mustPredicates);
            }
        }

        std::size_t stateOrdinal = 0U;
        for (const auto& [_, terminal] : terminals) {
            EventGroundPreset preset{};
            preset.sectionName = section.id.name.empty()
                ? "Section " + std::to_string(section.id.index)
                : section.id.name;
            preset.id = "sct:" + std::to_string(section.id.index) + ":" +
                std::to_string(section.startOffset) + ":" + std::to_string(stateOrdinal);
            for (const auto& predicate : terminal.mustPredicates) {
                if (!predicatesCommonToAll.contains(predicate)) preset.conditions.push_back(predicate);
            }
            preset.label = preset.sectionName;
            if (terminals.size() > 1U) {
                if (!preset.conditions.empty()) {
                    preset.label += " — ";
                    for (std::size_t index = 0; index < preset.conditions.size(); ++index) {
                        if (index != 0U) preset.label += " and ";
                        preset.label += preset.conditions[index];
                    }
                } else {
                    preset.label += " — State " + std::to_string(stateOrdinal + 1U);
                }
            }
            for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
                preset.assignments.push_back({ groups[groupIndex].key, terminal.states[groupIndex] });
            }
            result.presets.push_back(std::move(preset));
            ++stateOrdinal;
        }
    }
    return result;
}

void applyEventGroundPresetVisibility(
    const SceneModel& scene,
    const EventGroundPreset& preset,
    std::vector<std::uint8_t>& visibility) {
    for (const auto& group : scene.eventGroundGroups) {
        for (const auto& variant : group.variants) {
            for (const auto batchIndex : variant.batchIndices) {
                if (batchIndex < visibility.size()) visibility[batchIndex] = 0U;
            }
        }
    }
    for (const auto& assignment : preset.assignments) {
        if (assignment.state.kind != EventGroundStateKind::Variant) continue;
        const auto group = std::find_if(scene.eventGroundGroups.begin(), scene.eventGroundGroups.end(),
            [&](const EventGroundGroup& candidate) { return candidate.key == assignment.group; });
        if (group == scene.eventGroundGroups.end() || assignment.state.variantOrdinal >= group->variants.size()) continue;
        for (const auto batchIndex : group->variants[assignment.state.variantOrdinal].batchIndices) {
            if (batchIndex < visibility.size()) visibility[batchIndex] = 1U;
        }
    }
}

void applyRawEventGroundVisibility(
    const SceneModel& scene,
    std::vector<std::uint8_t>& visibility) {
    for (const auto& group : scene.eventGroundGroups) {
        for (const auto& variant : group.variants) {
            for (const auto batchIndex : variant.batchIndices) {
                if (batchIndex < visibility.size()) visibility[batchIndex] = 1U;
            }
        }
    }
}

} // namespace skewer::core
