#include "Slic3r/Biz/Preset/PrinterPresetSelector.hpp"

#include "Slic3r/Biz/Preset/PresetInteractor.hpp"
#include "Slic3r/Biz/Algorithms/Color.hpp"
#include "Slic3r/Biz/I18N/I18N.hpp"
#include "Slic3r/Domain/Preset/HwConfig.hpp"

#include "Slic3r/Log.hpp"
#include "nlohmann/json.hpp"
#include "fmt/format.h"

namespace Slic3r::Biz::Preset {

namespace {

// Connect reports the color as "#rrggbbaa", filament colors are "#rrggbb".
std::string material_color(const nlohmann::json& material)
{
    if (!material.contains("primary_color") || !material["primary_color"].is_object()) {
        return {};
    }
    const auto& primary_color = material["primary_color"];
    if (!primary_color.contains("color_rgba") || !primary_color["color_rgba"].is_string()) {
        return {};
    }
    const std::string rgba = primary_color["color_rgba"].get<std::string>();
    const std::string rgb  = rgba.substr(0, 7);
    if ((rgba.size() != 7 && rgba.size() != 9) || !Algorithms::Color::can_decode_color(rgb)) {
        return {};
    }
    return rgb;
}

} // namespace

std::string connect_tool_key(const Domain::Preset::HwPrinterConfig& hw_config, size_t slot_index)
{
    // Copy: address() refers into the iterator, which is a temporary here.
    const Domain::Preset::Address address =
        Domain::Preset::MaterialIterator::from_slot_index(hw_config, slot_index).address();
    std::string key;
    for (const uint8_t part : address) {
        if (!key.empty()) {
            key += '.';
        }
        key += std::to_string(part);
    }
    return key;
}

PrinterPresetSelector::PrinterPresetSelector(PresetInteractor& preset_interactor) :
    m_preset_interactor(preset_interactor)
{}

void PrinterPresetSelector::warn(const std::string& msg)
{
    this->invoke_listeners<IPrinterPresetSelectorListener>([&msg](auto* listener) {
        listener->on_printer_preset_selection_warning(msg);
    });
}

std::vector<std::string> PrinterPresetSelector::select_connect_materials(
    const nlohmann::json& printer_json,
    const MatDbMaterialSelector& select_matdb_material
)
{
    std::vector<std::string> mismatches;
    std::vector<std::string> colors =
        select_materials(printer_json, select_matdb_material, mismatches);
    warn_mismatches(mismatches);
    return colors;
}

void PrinterPresetSelector::warn_mismatches(const std::vector<std::string>& mismatches)
{
    if (mismatches.empty()) {
        return;
    }
    std::string msg;
    for (const std::string& mismatch : mismatches) {
        if (!msg.empty()) {
            msg += '\n';
        }
        msg += mismatch;
    }
    warn(msg);
}

std::vector<std::string> PrinterPresetSelector::select_materials(
    const nlohmann::json& printer_json,
    const MatDbMaterialSelector& select_matdb_material,
    std::vector<std::string>& mismatches
)
{
    if (!printer_json.contains("tools") || !printer_json["tools"].is_object()) {
        return {};
    }

    const auto& tools_json = printer_json["tools"];
    size_t max_slots = m_preset_interactor.material_presets().size();
    std::vector<std::string> colors(max_slots);

    const auto& selected_preset = m_preset_interactor.selected_printer_preset();
    const std::string& hw_config_id = selected_preset.hw_config.id;
    const std::string& printer_preset_id = selected_preset.printer.id;
    const std::string& print_preset_id = selected_preset.print.id;

    for (size_t slot_idx = 0; slot_idx < max_slots; ++slot_idx) {
        // A slot of a feeder (MMU) is reported under its address, e.g. "0.2".
        std::string tool_key = connect_tool_key(selected_preset.hw_config, slot_idx);

        if (!tools_json.contains(tool_key)) {
            continue;
        }

        const auto& tool_json = tools_json[tool_key];
        std::string target_type;
        std::string matdb_uuid;

        if (tool_json.contains("material_package_instance") && tool_json["material_package_instance"].is_object()) {
            const auto& mpi = tool_json["material_package_instance"];
            if (mpi.contains("package") && mpi["package"].is_object()) {
                const auto& pkg = mpi["package"];
                if (pkg.contains("material") && pkg["material"].is_object()) {
                    const auto& mat = pkg["material"];
                    if (mat.contains("type") && mat["type"].is_string()) {
                        target_type = mat["type"].get<std::string>();
                    } else if (mat.contains("abbreviation") && mat["abbreviation"].is_string()) {
                        // The type is optional in the material database, the abbreviation
                        // is not.
                        target_type = mat["abbreviation"].get<std::string>();
                    }
                    if (mat.contains("uuid") && mat["uuid"].is_string()) {
                        matdb_uuid = mat["uuid"].get<std::string>();
                    }
                    colors[slot_idx] = material_color(mat);
                }
            }
        }

        // A material from the material database (MDB) is selected as the user would select it
        // there, which brings its brand, name and color along. A slot that already holds the
        // material keeps its preset, the user may have picked another one offered for it.
        // The material type is matched only when the MDB selection is not possible.
        if (!matdb_uuid.empty() && select_matdb_material) {
            if (selected_preset.material_matdb_uuid(slot_idx) == matdb_uuid
                || select_matdb_material(slot_idx, matdb_uuid))
            {
                continue;
            }
        }

        if (target_type.empty()) {
            continue;
        }

        const auto& material_list       = m_preset_interactor.material_presets().at(slot_idx);
        const auto& available_materials = material_list.items();
        std::string matching_material_id;

        std::string best_prusament_id;
        size_t best_prusament_len = std::string::npos;
        std::string best_generic_id;
        size_t best_generic_len = std::string::npos;
        std::string first_match_id;

        for (size_t i = 0; i < available_materials.size(); ++i) {
            const PresetItem& preset = available_materials.at(i);

            for (const auto& [mat_preset, is_runtime] : m_preset_interactor.get_material_presets(hw_config_id, printer_preset_id, print_preset_id, slot_idx)) {
                if (is_runtime) {
                    continue;
                }

                if (mat_preset.get().id != preset.id) {
                    continue;
                }

                auto it = mat_preset.get().config_box().find("filament_type");
                if (it.item && it.item->template get<std::string>() == target_type) {
                    if (first_match_id.empty()) {
                        first_match_id = preset.id;
                    }

                    if (preset.name.starts_with("Prusament")) {
                        if (preset.name.length() < best_prusament_len) {
                            best_prusament_len = preset.name.length();
                            best_prusament_id = preset.id;
                        }
                    }
                    else if (preset.name.starts_with("Generic")) {
                        if (preset.name.length() < best_generic_len) {
                            best_generic_len = preset.name.length();
                            best_generic_id = preset.id;
                        }
                    }
                }
            }
        }

        if (!best_prusament_id.empty()) {
            matching_material_id = best_prusament_id;
        } else if (!best_generic_id.empty()) {
            matching_material_id = best_generic_id;
        } else {
            matching_material_id = first_match_id;
        }

        if (!matching_material_id.empty()) {
            // The empty MDB uuid drops an MDB material the slot may have had before.
            m_preset_interactor
                .select_material_preset(slot_idx, matching_material_id, false, std::string{});
        } else {
            SPDLOG_WARN("Connect material sync: No matching material preset found for slot {} (Target type: {})", slot_idx, target_type);
            // TRN Warning line, first {} is the material type, second {} the slot number.
            mismatches.push_back(fmt::format(fmt::runtime(_u8L("No material profile matches {} in slot {}.")), target_type, slot_idx + 1));
        }
    }
    return colors;
}

} // namespace Slic3r::Biz::Preset
