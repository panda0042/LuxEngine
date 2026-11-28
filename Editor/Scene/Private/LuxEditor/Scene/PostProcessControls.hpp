#pragma once

#include <LuxEngine/Renderer/Renderer3D/Render3DSettings.hpp>
#include <LuxUI/Widgets.hpp>

namespace LuxEditor {
// The dialog owns its render settings copy. Edits take effect when the scene dialog is accepted.
void AddPostProcessControls(LuxUI::VBoxLayout& layout, LuxUI::Widget& host,
                            LuxEngine::Render3DSettings& settings);
} // namespace LuxEditor
