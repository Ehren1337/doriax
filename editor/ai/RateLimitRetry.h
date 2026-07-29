#pragma once

#include "AiTypes.h"

#include <chrono>
#include <string>

namespace doriax::editor::ai {

// True only for a temporary HTTP 429. Permanent billing/quota failures use
// the same status but must be shown immediately instead of retried.
bool isRetryableRateLimit(long status, const ProviderResponse& error);

// Reads Retry-After, x-ratelimit-reset-tokens, or the provider's
// "try again in ..." detail. Returns zero when no usable hint is present.
std::chrono::milliseconds rateLimitRetryDelay(
    const HttpResponse& response, const std::string& detail);

} // namespace doriax::editor::ai
