// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "SecretStore.h"

#include "AppSettings.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace fs = std::filesystem;

namespace doriax::editor::ai {

namespace {

std::mutex g_mutex;
std::map<ProviderId, std::string> g_keys;
bool g_loaded = false;

fs::path keyFilePath() {
    return AppSettings::getConfigDirectory() / "ai_keys.dat";
}

uint64_t fnv1a(const std::string& value) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

// Seed bound to this machine/user (config dir path). Keys obfuscated with it are
// not portable to another machine and are not stored in plaintext.
uint64_t deviceSeed() {
    return fnv1a("doriax.ai.keys.v1|" + AppSettings::getConfigDirectory().string());
}

// Symmetric: applying it twice returns the original bytes.
std::string obfuscate(const std::string& data) {
    uint64_t state = deviceSeed();
    std::string out = data;
    for (char& c : out) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        c = static_cast<char>(static_cast<unsigned char>(c) ^ static_cast<unsigned char>(state & 0xFF));
    }
    return out;
}

std::string toHex(const std::string& data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char c : data) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0F]);
    }
    return out;
}

bool fromHex(const std::string& hex, std::string& out) {
    if (hex.size() % 2 != 0) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}

void loadLocked() {
    if (g_loaded) return;
    g_loaded = true;

    std::ifstream in(keyFilePath());
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string name = line.substr(0, eq);
        std::string hex = line.substr(eq + 1);
        trim(name);
        trim(hex);

        std::string obfuscated;
        if (!fromHex(hex, obfuscated)) continue;
        std::string key = obfuscate(obfuscated);
        if (!key.empty()) {
            g_keys[providerFromString(name)] = key;
        }
    }
}

void saveLocked() {
    fs::path path = keyFilePath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);

    out << "# Doriax AI API keys - obfuscated and machine-bound. Do not edit or share.\n";
    for (const auto& entry : g_keys) {
        if (entry.second.empty()) continue;
        out << toString(entry.first) << " = " << toHex(obfuscate(entry.second)) << "\n";
    }
}

} // namespace

void SecretStore::setApiKey(ProviderId provider, const std::string& key) {
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();
    if (key.empty()) {
        g_keys.erase(provider);
    } else {
        g_keys[provider] = key;
    }
    saveLocked();
}

std::string SecretStore::getApiKey(ProviderId provider) {
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();
    auto it = g_keys.find(provider);
    return it == g_keys.end() ? std::string() : it->second;
}

bool SecretStore::hasApiKey(ProviderId provider) {
    return !getApiKey(provider).empty();
}

void SecretStore::clearApiKey(ProviderId provider) {
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();
    g_keys.erase(provider);
    saveLocked();
}

std::vector<ProviderId> SecretStore::providersWithKeys() {
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();
    std::vector<ProviderId> result;
    for (ProviderId provider : {ProviderId::OpenAI, ProviderId::Anthropic,
                                ProviderId::Gemini, ProviderId::DeepSeek,
                                ProviderId::OpenAICompatible}) {
        auto it = g_keys.find(provider);
        if (it != g_keys.end() && !it->second.empty()) {
            result.push_back(provider);
        }
    }
    return result;
}

} // namespace doriax::editor::ai
