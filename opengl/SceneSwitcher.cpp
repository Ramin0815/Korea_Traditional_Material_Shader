#include "SceneSwitcher.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace
{
constexpr float LeftPanelWidth = 224.0f;
constexpr float RightPanelWidth = 118.0f;
constexpr float PanelGap = 7.0f;

constexpr ImGuiWindowFlags PanelFlags =
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

void beginFixedPanel(const char* name, const ImVec2& position, const ImVec2& size,
                     ImGuiWindowFlags extraFlags = 0)
{
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin(name, nullptr, PanelFlags | extraFlags);
}

void drawSphereIcon(ImDrawList* draw, const ImVec2& center, float radius, ImU32 color)
{
    draw->AddCircle(center, radius, color, 32, 1.8f);
    draw->AddEllipse(center, ImVec2(radius * 0.43f, radius), color, 0.0f, 24, 1.1f);
    draw->AddEllipse(center, ImVec2(radius, radius * 0.36f), color, 0.0f, 24, 1.1f);
}

void drawSquareIcon(ImDrawList* draw, const ImVec2& center, float radius, ImU32 color)
{
    const float d = radius * 0.45f;
    const ImVec2 a(center.x - radius, center.y - radius * 0.58f);
    const ImVec2 b(center.x + d, center.y - radius);
    const ImVec2 c(center.x + radius, center.y + radius * 0.40f);
    const ImVec2 e(center.x - d, center.y + radius);
    draw->AddQuad(a, b, c, e, color, 1.8f);
    const ImVec2 top(center.x, center.y - radius * 0.10f);
    draw->AddLine(a, top, color, 1.2f);
    draw->AddLine(b, top, color, 1.2f);
    draw->AddLine(c, top, color, 1.2f);
    draw->AddLine(e, top, color, 1.2f);
}

void drawCylinderIcon(ImDrawList* draw, const ImVec2& center, float radius, ImU32 color)
{
    const ImVec2 top(center.x, center.y - radius * 0.58f);
    const ImVec2 bottom(center.x, center.y + radius * 0.58f);
    const ImVec2 ellipseRadius(radius, radius * 0.38f);
    draw->AddEllipse(top, ellipseRadius, color, 0.0f, 28, 1.8f);
    draw->AddEllipse(bottom, ellipseRadius, color, 0.0f, 28, 1.8f);
    draw->AddLine(ImVec2(center.x - radius, top.y), ImVec2(center.x - radius, bottom.y), color, 1.8f);
    draw->AddLine(ImVec2(center.x + radius, top.y), ImVec2(center.x + radius, bottom.y), color, 1.8f);
}

bool modelButton(const char* id, const char* label, ModelSelection value,
                 ModelSelection selected, void (*drawIcon)(ImDrawList*, const ImVec2&, float, ImU32))
{
    const float width = std::max(66.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 size(width, 82.0f);
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = selected == value;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 maximum(minimum.x + size.x, minimum.y + size.y);
    const ImU32 background = ImGui::GetColorU32(active ? ImVec4(0.12f, 0.38f, 0.58f, 1.0f)
                                                       : hovered ? ImVec4(0.20f, 0.22f, 0.25f, 1.0f)
                                                                 : ImVec4(0.13f, 0.14f, 0.16f, 1.0f));
    const ImU32 border = ImGui::GetColorU32(active ? ImVec4(0.30f, 0.72f, 1.0f, 1.0f)
                                                   : ImVec4(0.32f, 0.34f, 0.37f, 1.0f));
    draw->AddRectFilled(minimum, maximum, background, 5.0f);
    draw->AddRect(minimum, maximum, border, 5.0f, 0, active ? 2.0f : 1.0f);
    drawIcon(draw, ImVec2((minimum.x + maximum.x) * 0.5f, minimum.y + 31.0f), 19.0f,
             ImGui::GetColorU32(ImVec4(0.88f, 0.91f, 0.94f, 1.0f)));
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(minimum.x + (size.x - textSize.x) * 0.5f, minimum.y + 60.0f),
                  ImGui::GetColorU32(ImGuiCol_Text), label);
    return clicked;
}

bool baseColorButton(const char* id, const char* label, JagaeBaseColor value,
                     JagaeBaseColor selected, const ImVec4& swatchColor,
                     const char* tooltip)
{
    const float width = std::max(80.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 size(width, 31.0f);
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = selected == value;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 maximum(minimum.x + size.x, minimum.y + size.y);
    const ImU32 background = ImGui::GetColorU32(active ? ImVec4(0.12f, 0.38f, 0.58f, 1.0f)
                                                       : hovered ? ImVec4(0.20f, 0.22f, 0.25f, 1.0f)
                                                                 : ImVec4(0.13f, 0.14f, 0.16f, 1.0f));
    const ImU32 border = ImGui::GetColorU32(active ? ImVec4(0.30f, 0.72f, 1.0f, 1.0f)
                                                   : ImVec4(0.32f, 0.34f, 0.37f, 1.0f));
    draw->AddRectFilled(minimum, maximum, background, 4.0f);
    draw->AddRect(minimum, maximum, border, 4.0f, 0, active ? 2.0f : 1.0f);
    const ImVec2 swatchMin(minimum.x + 9.0f, minimum.y + 8.0f);
    const ImVec2 swatchMax(swatchMin.x + 15.0f, swatchMin.y + 15.0f);
    draw->AddRectFilled(swatchMin, swatchMax, ImGui::GetColorU32(swatchColor), 3.0f);
    draw->AddRect(swatchMin, swatchMax, ImGui::GetColorU32(ImVec4(0.76f, 0.78f, 0.80f, 1.0f)), 3.0f);
    draw->AddText(ImVec2(minimum.x + 33.0f, minimum.y + 8.0f),
                  ImGui::GetColorU32(ImGuiCol_Text), label);
    if (hovered)
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }
    return clicked;
}
}

RenderViewport SceneSwitcher::draw(int framebufferWidth, int framebufferHeight,
                                   GLuint renderTexture, SceneSelection& material,
                                   ModelSelection& model, DirectionalLight& jagaeLight,
                                   JagaeBaseColor& baseColor, bool& clearCoatEnabled, EnvironmentPreset& environment,
                                   const std::string& environmentError, float nacreStructuralMix,
                                   JagaeLightMode* lightMode, bool* patternEnabled) const
{
    const ImGuiViewport* root = ImGui::GetMainViewport();
    const ImVec2 origin = root->WorkPos;
    const ImVec2 extent = root->WorkSize;
    const float leftWidth = std::min(LeftPanelWidth, extent.x * 0.26f);
    const float rightWidth = std::min(RightPanelWidth, extent.x * 0.18f);
    const float centerWidth = std::max(1.0f, extent.x - leftWidth - rightWidth - PanelGap * 2.0f);

    beginFixedPanel("Materials", origin, ImVec2(leftWidth, extent.y));
    ImGui::TextDisabled("MATERIAL LIBRARY");
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent(8.0f);
        if (ImGui::Selectable("Gat", material == SceneSelection::Gat))
            material = SceneSelection::Gat;
        if (ImGui::Selectable("Jagae", material == SceneSelection::Jagae))
            material = SceneSelection::Jagae;
        ImGui::Unindent(8.0f);
    }
    if (material == SceneSelection::Jagae && ImGui::CollapsingHeader("Base Color", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent(8.0f);
        if (baseColorButton("##jagaeBaseBlack", "Black", JagaeBaseColor::Black, baseColor,
                            ImVec4(0.035f, 0.04f, 0.05f, 1.0f),
                            "Black body tint; optical mix is 45/55 body/structural. Pattern-on outside: unlit light gray (.80 linear, ~231 default display); Studio fill: .06."))
            baseColor = JagaeBaseColor::Black;
        if (baseColorButton("##jagaeBaseWhite", "White", JagaeBaseColor::White, baseColor,
                            ImVec4(0.92f, 0.92f, 0.89f, 1.0f),
                            "White body tint; optical mix is 45/55 body/structural. Pattern-on outside: unlit dark gray (.025 linear, ~44 default display); Studio fill: .80."))
            baseColor = JagaeBaseColor::White;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Optical mix: %.0f%% body / %.0f%% structural; lighting and pattern outside color vary by preset.",
            (1.0f - nacreStructuralMix) * 100.0f, nacreStructuralMix * 100.0f);
        ImGui::PopTextWrapPos();
        ImGui::Unindent(8.0f);
    }
    if (material == SceneSelection::Jagae && patternEnabled != nullptr
        && ImGui::CollapsingHeader("Pattern", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent(8.0f);
        ImGui::Checkbox("Enable Pattern", patternEnabled);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("PNG alpha confines the inlay and highlights; the outside is unlit light gray for Black and dark gray for White.");
        ImGui::PopTextWrapPos();
        ImGui::Unindent(8.0f);
    }
    if (material == SceneSelection::Jagae && ImGui::CollapsingHeader("Clear Coat", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Enable Clear Coat", &clearCoatEnabled);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Coat mask: %.2f", clearCoatEnabled ? .25f : 0.0f);
        ImGui::TextDisabled("Other reflections and highlights can remain.");
        ImGui::PopTextWrapPos();
    }
    if (material == SceneSelection::Jagae && ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if(ImGui::RadioButton("Neutral studio",environment==EnvironmentPreset::Studio)) environment=EnvironmentPreset::Studio;
        if(ImGui::RadioButton("Unity reference sky",environment==EnvironmentPreset::UnityReference)) environment=EnvironmentPreset::UnityReference;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Environment stays fixed. Arrows rotate the selected direct light.");
        if(!environmentError.empty()) ImGui::TextColored(ImVec4(1,.65f,.3f,1),"%s",environmentError.c_str());
        ImGui::PopTextWrapPos();
    }
    if (material == SceneSelection::Jagae && ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if(lightMode) {
            if(ImGui::RadioButton("Directional Light",*lightMode==JagaeLightMode::Directional)) *lightMode=JagaeLightMode::Directional;
            if(ImGui::RadioButton("Area Light",*lightMode==JagaeLightMode::Area)) *lightMode=JagaeLightMode::Area;
        }
        float angles[2] = { jagaeLight.azimuthDegrees(), jagaeLight.elevationDegrees() };
        ImGui::TextUnformatted("Azimuth");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##Azimuth", &angles[0], 0.0f, 360.0f, "%.1f deg")) jagaeLight.setAzimuthDegrees(angles[0]);
        ImGui::TextUnformatted("Elevation");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##Elevation", &angles[1], -89.0f, 89.0f, "%.1f deg")) jagaeLight.setElevationDegrees(angles[1]);
        if (ImGui::Button("Reset light")) jagaeLight.reset();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Arrows rotate; Ctrl: 0.1 deg, Shift: 5 deg. R resets while the viewport is focused.");
        ImGui::PopTextWrapPos();
    }
    ImGui::End();

    const ImVec2 centerPosition(origin.x + leftWidth + PanelGap, origin.y);
    beginFixedPanel("Render View", centerPosition, ImVec2(centerWidth, extent.y),
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const bool resetView = ImGui::Button("Reset view");
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 imageSize(std::max(1.0f, available.x), std::max(1.0f, available.y));
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(renderTexture)), imageSize,
                 ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    const bool viewportHovered = ImGui::IsItemHovered();
    if (material == SceneSelection::Jagae)
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetItemRectMin();
        const ImVec2 s = ImGui::GetItemRectSize();
        const glm::vec3 d = jagaeLight.direction();
        draw->PushClipRect(p, ImVec2(p.x + s.x, p.y + s.y), true);
        const ImVec2 start(p.x + s.x - 74.0f, p.y + 66.0f);
        const float projectionLength = std::sqrt(d.x * d.x + d.y * d.y);
        const ImVec2 axis = projectionLength > 0.001f
            ? ImVec2(d.x / projectionLength, -d.y / projectionLength) : ImVec2(0.0f, 0.0f);
        const ImVec2 end(start.x + axis.x * 28.0f, start.y + axis.y * 28.0f);
        const ImU32 ink = IM_COL32(235, 239, 245, 220);
        if (projectionLength > 0.001f)
        {
            draw->AddLine(start, end, ink, 2.0f);
            const ImVec2 perpendicular(-axis.y, axis.x);
            draw->AddTriangleFilled(end,
                ImVec2(end.x - axis.x * 8.0f + perpendicular.x * 4.0f, end.y - axis.y * 8.0f + perpendicular.y * 4.0f),
                ImVec2(end.x - axis.x * 8.0f - perpendicular.x * 4.0f, end.y - axis.y * 8.0f - perpendicular.y * 4.0f), ink);
        }
        else draw->AddCircleFilled(start, 3.0f, ink);
        const char* depthSign = d.z < 0.0f ? "Z-" : "Z+";
        draw->AddText(ImVec2(p.x + s.x - 120.0f, p.y + 12.0f), ink, "LIGHT XY PROJECTION");
        draw->AddText(ImVec2(p.x + s.x - 40.0f, p.y + 28.0f), ink, depthSign);
        draw->PopClipRect();
    }
    ImGui::End();

    const ImVec2 rightPosition(centerPosition.x + centerWidth + PanelGap, origin.y);
    beginFixedPanel("Models", rightPosition, ImVec2(rightWidth, extent.y));
    ImGui::TextDisabled("MESH");
    ImGui::Separator();
    ImGui::Spacing();
    if (modelButton("##sphere", "SPHERE", ModelSelection::Sphere, model, drawSphereIcon))
        model = ModelSelection::Sphere;
    ImGui::Spacing();
    if (modelButton("##square", "SQUARE", ModelSelection::Square, model, drawSquareIcon))
        model = ModelSelection::Square;
    ImGui::Spacing();
    if (modelButton("##cylinder", "CYLINDER", ModelSelection::Cylinder, model, drawCylinderIcon))
        model = ModelSelection::Cylinder;
    ImGui::End();

    const float scaleX = extent.x > 0.0f ? static_cast<float>(framebufferWidth) / extent.x : 1.0f;
    const float scaleY = extent.y > 0.0f ? static_cast<float>(framebufferHeight) / extent.y : 1.0f;
    return {std::max(1, static_cast<int>(std::lround(imageSize.x * scaleX))),
            std::max(1, static_cast<int>(std::lround(imageSize.y * scaleY))),
            viewportHovered, resetView};
}
