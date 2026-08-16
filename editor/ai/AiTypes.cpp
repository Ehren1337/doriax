// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "AiTypes.h"

namespace doriax::editor::ai {

std::string toString(ProviderId provider) {
    switch (provider) {
        case ProviderId::OpenAI: return "openai";
        case ProviderId::Anthropic: return "anthropic";
        case ProviderId::Gemini: return "gemini";
        case ProviderId::DeepSeek: return "deepseek";
        case ProviderId::OpenAICompatible: return "openai_compatible";
    }
    return "openai";
}

ProviderId providerFromString(const std::string& value) {
    if (value == "anthropic") return ProviderId::Anthropic;
    if (value == "gemini") return ProviderId::Gemini;
    if (value == "deepseek") return ProviderId::DeepSeek;
    if (value == "openai_compatible") return ProviderId::OpenAICompatible;
    return ProviderId::OpenAI;
}

std::string defaultModelForProvider(ProviderId provider) {
    switch (provider) {
        case ProviderId::OpenAI: return "gpt-4.1";
        case ProviderId::Anthropic: return "claude-sonnet-4-20250514";
        case ProviderId::Gemini: return "gemini-2.5-flash"; // Flash has a free tier; Pro does not
        case ProviderId::DeepSeek: return "deepseek-chat";
        case ProviderId::OpenAICompatible: return "openai/gpt-4.1";
    }
    return "gpt-4.1";
}

std::string toString(ChatRole role) {
    switch (role) {
        case ChatRole::System: return "system";
        case ChatRole::User: return "user";
        case ChatRole::Assistant: return "assistant";
        case ChatRole::Tool: return "tool";
    }
    return "user";
}

ChatRole chatRoleFromString(const std::string& value) {
    if (value == "system") return ChatRole::System;
    if (value == "assistant") return ChatRole::Assistant;
    if (value == "tool") return ChatRole::Tool;
    return ChatRole::User;
}

std::string toString(ApprovalMode mode) {
    switch (mode) {
        case ApprovalMode::PreviewThenApprove: return "preview_then_approve";
        case ApprovalMode::SafeAutoRun: return "safe_auto_run";
        case ApprovalMode::FullAgent: return "full_agent";
    }
    return "preview_then_approve";
}

ApprovalMode approvalModeFromString(const std::string& value) {
    if (value == "safe_auto_run") return ApprovalMode::SafeAutoRun;
    if (value == "full_agent") return ApprovalMode::FullAgent;
    return ApprovalMode::PreviewThenApprove;
}

} // namespace doriax::editor::ai
