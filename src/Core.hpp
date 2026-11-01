// Copyright (C) 2026 AutoGavy
// SPDX-License-Identifier: AGPL-3.0-only
// See LICENSE in the project root for the full license text.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace regionlock {
inline constexpr wchar_t kRuleGroup[] = L"RegionLock.Native.v1";
inline constexpr wchar_t kRulePrefix[] = L"RegionLock.Native.v1.";
inline constexpr std::uint16_t kFirstPort = 21000;
inline constexpr std::uint16_t kLastPort = 21299;

struct Prefix {
    std::array<std::uint8_t, 16> address{};
    std::uint8_t length{};
    bool ipv6{};
    bool operator==(const Prefix&) const = default;
};
struct Region {
    std::string code;
    std::wstring name;
    std::vector<Prefix> prefixes;
};
struct Catalog {
    std::string created;
    std::vector<Region> regions;
    std::size_t ipv4Count{};
    std::size_t ipv6Count{};
};
struct RuleSpec {
    std::wstring name;
    std::wstring application;
    std::wstring remoteAddresses;
    std::uint16_t firstPort{kFirstPort};
    std::uint16_t lastPort{kLastPort};
};

std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);
std::optional<Prefix> ParsePrefix(std::string_view text);
std::string PrefixText(const Prefix& prefix);
std::vector<Prefix> CompactPrefixes(std::vector<Prefix> prefixes);
Catalog ParseCatalog(std::string_view json);
std::vector<RuleSpec> BuildRules(const Catalog& catalog,
    const std::vector<std::string>& allowedRegions, std::wstring_view application,
    std::wstring_view generation);
}
