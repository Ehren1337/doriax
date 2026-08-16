// (c) Eduardo Doria Lima
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"

#include <array>

namespace doriax::editor {

namespace ai { class AiService; }

// Modal dialog that manages API keys for every provider (add one, switch
// provider, add another - like VS Code) plus the OpenAI-compatible endpoint and
// request limits. Model and approval controls live in the chat composer.
class AiSettingsWindow {
private:
    static constexpr int kProviderCount = 5;

    bool m_isOpen = false;
    ai::AiService* m_service = nullptr;

    std::array<std::array<char, 512>, kProviderCount> m_keyBuffers{};
    std::array<bool, kProviderCount> m_keySet{};
    std::array<char, 512> m_endpointBuffer{};
    int m_requestTimeoutSeconds = 90;
    int m_maxOutputTokens = 8192;
    int m_maxToolRounds = 24;

    void drawSettings();
    void refreshKeyState();
    void apply();

public:
    AiSettingsWindow() = default;
    ~AiSettingsWindow() = default;

    void open(ai::AiService* service);
    void show();
    bool isOpen() const { return m_isOpen; }
};

} // namespace doriax::editor
