// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "AiSettingsWindow.h"

#include "AppSettings.h"
#include "ai/AiService.h"
#include "ai/AiTypes.h"
#include "ai/SecretStore.h"
#include "external/IconsFontAwesome6.h"
#include "window/Widgets.h"

#include <algorithm>
#include <cstdio>

namespace doriax::editor {

namespace {

const char* kProviderLabels[] = {"OpenAI", "Anthropic", "Gemini", "DeepSeek", "OpenAI-compatible"};
const ai::ProviderId kProviderValues[] = {
    ai::ProviderId::OpenAI,
    ai::ProviderId::Anthropic,
    ai::ProviderId::Gemini,
    ai::ProviderId::DeepSeek,
    ai::ProviderId::OpenAICompatible
};

const ImVec4 kKeySetColor(0.55f, 0.85f, 0.55f, 1.0f);

} // namespace

void AiSettingsWindow::open(ai::AiService* service) {
    m_isOpen = true;
    m_service = service;

    ai::Settings settings = AppSettings::getAiSettings();
    m_requestTimeoutSeconds = settings.requestTimeoutSeconds;
    m_maxOutputTokens = settings.maxOutputTokens;
    m_maxToolRounds = settings.maxToolRounds;
    std::snprintf(m_endpointBuffer.data(), m_endpointBuffer.size(), "%s", settings.customEndpoint.c_str());
    for (auto& buffer : m_keyBuffers) {
        buffer.fill('\0');
    }
    refreshKeyState();
}

void AiSettingsWindow::refreshKeyState() {
    static_assert(IM_ARRAYSIZE(kProviderValues) == kProviderCount,
                  "kProviderValues/kProviderLabels must list every provider");
    static_assert(IM_ARRAYSIZE(kProviderLabels) == kProviderCount,
                  "kProviderValues/kProviderLabels must list every provider");
    for (int i = 0; i < kProviderCount; ++i) {
        m_keySet[i] = ai::SecretStore::hasApiKey(kProviderValues[i]);
    }
}

void AiSettingsWindow::apply() {
    ai::Settings settings = AppSettings::getAiSettings();
    settings.customEndpoint = m_endpointBuffer.data();
    settings.requestTimeoutSeconds = std::clamp(m_requestTimeoutSeconds, 1, 3600);
    settings.maxOutputTokens = std::clamp(m_maxOutputTokens, 256, 16000);
    settings.maxToolRounds = std::clamp(m_maxToolRounds, 1, 100);
    AppSettings::setAiSettings(settings);
    if (m_service) {
        m_service->setSettings(settings);
    }

    // Persist any keys the user typed (one per provider). Keys are stored
    // obfuscated in the user config dir, never alongside the project.
    for (int i = 0; i < kProviderCount; ++i) {
        if (m_keyBuffers[i][0] != '\0') {
            ai::SecretStore::setApiKey(kProviderValues[i], m_keyBuffers[i].data());
            m_keyBuffers[i].fill('\0');
        }
    }
    refreshKeyState();
}

void AiSettingsWindow::show() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("AI Settings##AiSettingsModal");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(560, 0),
        ImVec2(560, ImGui::GetMainViewport()->WorkSize.y * 0.9f)
    );

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::BeginPopupModal("AI Settings##AiSettingsModal", &m_isOpen, flags)) {
        if (!m_isOpen) {
            ImGui::CloseCurrentPopup();
        } else {
            drawSettings();
        }
        ImGui::EndPopup();
    }
}

void AiSettingsWindow::drawSettings() {
    ImGui::TextDisabled("Add a key for any provider; keys are stored obfuscated in your");
    ImGui::TextDisabled("user config folder. The model picker lists configured providers.");
    ImGui::Spacing();

    // Default-constructed settings carry the field defaults (kept in sync with AiTypes.h).
    const ai::Settings defaults;

    // Borderless rotate-left "reset to default" button placed after a label, mirroring the
    // pattern in Properties.cpp. Returns true when clicked.
    auto resetToDefaultButton = [](const char* id, const std::string& tooltip) -> bool {
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        bool clicked = ImGui::Button((std::string(ICON_FA_ROTATE_LEFT) + "##" + id).c_str());
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip.c_str());
        }
        return clicked;
    };

    ImGui::BeginTable("ai_settings", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp);
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 160);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

    float clearWidth = ImGui::GetFrameHeight();
    float spacing = ImGui::GetStyle().ItemSpacing.x;

    for (int i = 0; i < kProviderCount; ++i) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", kProviderLabels[i]);
        if (m_keySet[i]) {
            ImGui::SameLine();
            ImGui::TextColored(kKeySetColor, ICON_FA_CIRCLE_CHECK);
            ImGui::SetItemTooltip("Key configured");
        }

        ImGui::TableNextColumn();
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearWidth - spacing);
        ImGui::InputTextWithHint("##Key", m_keySet[i] ? "key set - type to replace" : "paste API key",
                                 m_keyBuffers[i].data(), m_keyBuffers[i].size(),
                                 ImGuiInputTextFlags_Password);
        if (ImGui::BeginPopupContextItem("##KeyContext")) {
            if (ImGui::MenuItem(ICON_FA_CLIPBOARD " Paste")) {
                const char* clipboard = ImGui::GetClipboardText();
                if (clipboard) {
                    std::snprintf(m_keyBuffers[i].data(), m_keyBuffers[i].size(), "%s", clipboard);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_keySet[i] && m_keyBuffers[i][0] == '\0');
        if (Widgets::iconButton("##ClearKey", ICON_FA_TRASH, ImVec2(clearWidth, ImGui::GetFrameHeight()))) {
            ai::SecretStore::clearApiKey(kProviderValues[i]);
            m_keyBuffers[i].fill('\0');
            refreshKeyState();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Clear this provider's key");
        ImGui::PopID();
    }

    // OpenAI-compatible endpoint
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Compatible endpoint");
    ImGui::SetItemTooltip("Chat Completions URL for the OpenAI-compatible provider. Empty = OpenRouter.");
    if (m_endpointBuffer[0] != '\0' && resetToDefaultButton("reset_endpoint", "Reset to default (empty)")) {
        m_endpointBuffer.fill('\0');
    }
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##Endpoint", "https://openrouter.ai/api/v1/chat/completions",
                             m_endpointBuffer.data(), m_endpointBuffer.size());

    // Per-request timeout
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Request Timeout (s)");
    ImGui::SetItemTooltip(
        "Maximum time to wait for each AI model response. Raise this for slow local models.");
    if (m_requestTimeoutSeconds != defaults.requestTimeoutSeconds &&
        resetToDefaultButton("reset_requesttimeout",
                             "Reset to default (" + std::to_string(defaults.requestTimeoutSeconds) + " seconds)")) {
        m_requestTimeoutSeconds = defaults.requestTimeoutSeconds;
    }
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputInt("##RequestTimeout", &m_requestTimeoutSeconds, 30, 60);
    m_requestTimeoutSeconds = std::clamp(m_requestTimeoutSeconds, 1, 3600);

    // Max output tokens
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Max Output Tokens");
    if (m_maxOutputTokens != defaults.maxOutputTokens &&
        resetToDefaultButton("reset_maxoutput", "Reset to default (" + std::to_string(defaults.maxOutputTokens) + ")")) {
        m_maxOutputTokens = defaults.maxOutputTokens;
    }
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputInt("##MaxOutput", &m_maxOutputTokens, 256, 1024);
    m_maxOutputTokens = std::clamp(m_maxOutputTokens, 256, 16000);

    // Max tool steps (agent loop continuations after tool results)
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Max Tool Steps");
    ImGui::SetItemTooltip(
        "How many model turns that request tools are allowed per user message. "
        "Raise this if you see \"Reached the tool-step limit\" on long tasks.");
    if (m_maxToolRounds != defaults.maxToolRounds &&
        resetToDefaultButton("reset_maxtoolrounds",
                             "Reset to default (" + std::to_string(defaults.maxToolRounds) + ")")) {
        m_maxToolRounds = defaults.maxToolRounds;
    }
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputInt("##MaxToolRounds", &m_maxToolRounds, 1, 4);
    m_maxToolRounds = std::clamp(m_maxToolRounds, 1, 100);

    ImGui::EndTable();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float windowWidth = ImGui::GetWindowSize().x;
    float buttonsWidth = 250;
    ImGui::SetCursorPosX((windowWidth - buttonsWidth) * 0.5f);

    if (ImGui::Button("OK", ImVec2(120, 0))) {
        apply();
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }
}

} // namespace doriax::editor
