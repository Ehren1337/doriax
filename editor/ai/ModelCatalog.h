// (c) Eduardo Doria Lima
// SPDX-License-Identifier: MIT

#pragma once

#include "AiTypes.h"
#include "HttpClient.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace doriax::editor::ai {

struct ModelInfo {
    std::string id;
    std::string label;
};

enum class CatalogStatus { Idle, Loading, Loaded, Error };

// Fetches the list of models each provider exposes (OpenAI /v1/models,
// Anthropic /v1/models, Gemini ListModels, DeepSeek /models,
// OpenAI-compatible /models) on a background thread and caches the result.
// Falls back to a curated list when nothing has been fetched yet or a request
// fails.
class ModelCatalog {
public:
    ModelCatalog();
    ~ModelCatalog();

    // Cached models for a provider, or the curated fallback if not yet loaded.
    std::vector<ModelInfo> models(ProviderId provider) const;
    CatalogStatus status(ProviderId provider) const;

    // Queues a background fetch (forces a re-fetch even if already loaded).
    void refresh(ProviderId provider, const std::string& apiKey, const std::string& endpoint);

    // Builds a friendly label from a raw model id when a provider exposes no
    // display name (e.g. "deepseek-v4-flash" -> "Deepseek V4 Flash").
    static std::string humanizeModelId(const std::string& id);

private:
    struct Request {
        ProviderId provider;
        std::string apiKey;
        std::string endpoint;
    };
    struct Entry {
        CatalogStatus status = CatalogStatus::Idle;
        std::vector<ModelInfo> models;
    };

    void workerLoop();
    bool fetchModels(const Request& request, std::vector<ModelInfo>& out);

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::map<ProviderId, Entry> cache;
    std::deque<Request> queue;
    std::thread worker;
    bool stop = false;
    HttpClient httpClient;
};

} // namespace doriax::editor::ai
