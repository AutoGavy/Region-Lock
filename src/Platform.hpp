// Copyright (C) 2026 AutoGavy
// SPDX-License-Identifier: AGPL-3.0-only
// See LICENSE in the project root for the full license text.

#pragma once

#include "Core.hpp"
#include <filesystem>
#include <stdexcept>
#include <stop_token>

namespace regionlock {
class OperationCancelled final : public std::runtime_error {
public:
    OperationCancelled() : std::runtime_error("Operation canceled") {}
};

struct Paths {
    std::filesystem::path directory;
    std::filesystem::path settingsFile;
    std::filesystem::path catalogFile;
};
struct Settings {
    std::wstring gamePath;
    std::vector<std::string> allowedRegions;
};
struct FirewallState {
    std::size_t ruleCount{};
    bool firewallEnabled{};
    std::wstring detail;
    std::size_t legacyRuleCount{};
    bool verified{true};
};
struct ProbeResult {
    std::string code;
    int received{};
    int total{5};
    double medianMs{};
    std::wstring error;
};

Paths GetPaths();
Settings LoadSettings(const Paths& paths);
void SaveSettings(const Paths& paths, const Settings& settings);
std::string ReadFile(const std::filesystem::path& path, std::size_t limit = 16 * 1024 * 1024);
void WriteFileAtomic(const std::filesystem::path& path, std::string_view bytes);
std::string DownloadCatalog(std::stop_token stop);
std::wstring FindGameExecutable();
std::wstring ValidateGameExecutable(std::wstring_view path);
std::wstring NewGeneration();
FirewallState QueryFirewall();
void ApplyFirewallRules(const std::vector<RuleSpec>& rules);
void RemoveOwnedRules();
ProbeResult ProbeRegion(const std::string& code, std::stop_token stop);
}
