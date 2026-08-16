// (c) Eduardo Doria Lima
// SPDX-License-Identifier: MIT

#pragma once

#include "AiTypes.h"

#include <string>
#include <vector>

namespace doriax::editor::ai {

// Per-provider API key store. Keys are persisted to a 0600 file in the user
// config directory, lightly obfuscated with a machine-derived keystream so they
// are not written in plaintext. This protects against casual reads and is not as
// strong as an OS keychain. Multiple providers can each hold their own key.
class SecretStore {
public:
    static void setApiKey(ProviderId provider, const std::string& key);
    static std::string getApiKey(ProviderId provider);
    static bool hasApiKey(ProviderId provider);
    static void clearApiKey(ProviderId provider);

    // Providers that currently have a non-empty key, in enum order.
    static std::vector<ProviderId> providersWithKeys();
};

} // namespace doriax::editor::ai
