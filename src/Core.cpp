// Copyright (C) 2026 AutoGavy
// SPDX-License-Identifier: AGPL-3.0-only
// See LICENSE in the project root for the full license text.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "Core.hpp"
#include "yyjson.h"

#include <algorithm>
#include <charconv>
#include <climits>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_set>

namespace regionlock {
namespace {
constexpr std::size_t kMaximumJsonBytes = 32 * 1024 * 1024;
constexpr std::size_t kMaximumEntries = 200000;
constexpr std::size_t kMaximumRegions = 256;
constexpr std::size_t kMaximumAddressesPerRule = 400;
constexpr std::size_t kMaximumAddressCharacters = 16000;

[[noreturn]] void Invalid(const char* message) { throw std::invalid_argument(message); }

bool PrefixLess(const Prefix& a, const Prefix& b) {
    if (a.ipv6 != b.ipv6) return a.ipv6 < b.ipv6;
    if (a.address != b.address) return a.address < b.address;
    return a.length < b.length;
}

Prefix Normalize(Prefix prefix) {
    const unsigned bits = prefix.ipv6 ? 128U : 32U;
    if (prefix.length > bits) Invalid("Invalid IP prefix length.");
    const unsigned full = prefix.length / 8;
    const unsigned remainder = prefix.length % 8;
    if (remainder) prefix.address[full] &= static_cast<std::uint8_t>(0xFFU << (8 - remainder));
    std::fill(prefix.address.begin() + full + (remainder ? 1 : 0), prefix.address.end(), std::uint8_t{});
    return prefix;
}

bool Contains(const Prefix& outer, const Prefix& inner) {
    if (outer.ipv6 != inner.ipv6 || outer.length > inner.length) return false;
    const unsigned full = outer.length / 8;
    if (!std::equal(outer.address.begin(), outer.address.begin() + full, inner.address.begin())) return false;
    const unsigned remainder = outer.length % 8;
    return !remainder || ((outer.address[full] ^ inner.address[full]) & (0xFFU << (8 - remainder))) == 0;
}

bool RegionCodeValid(std::string_view code) {
    if (code == "GLOBAL") return true;
    if (code.empty() || code.size() > 64 || code.front() == '-' || code.back() == '-') return false;
    return std::all_of(code.begin(), code.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

std::wstring RegionName(std::string_view code) {
    static const std::map<std::string_view, std::wstring_view> names{
        {"af-south-1", L"Cape Town"}, {"ap-east-1", L"Hong Kong"},
        {"ap-east-2", L"Taipei"}, {"ap-northeast-1", L"Tokyo"},
        {"ap-northeast-2", L"Seoul"}, {"ap-northeast-3", L"Osaka"},
        {"ap-south-1", L"Mumbai"}, {"ap-south-2", L"Hyderabad"},
        {"ap-southeast-1", L"Singapore"}, {"ap-southeast-2", L"Sydney"},
        {"ap-southeast-3", L"Jakarta"}, {"ap-southeast-4", L"Melbourne"},
        {"ap-southeast-5", L"Malaysia"}, {"ap-southeast-6", L"Auckland"},
        {"ap-southeast-7", L"Thailand"}, {"ca-central-1", L"Canada Central"},
        {"ca-west-1", L"Calgary"}, {"cn-north-1", L"Beijing"},
        {"cn-northwest-1", L"Ningxia"}, {"eu-central-1", L"Frankfurt"},
        {"eu-central-2", L"Zurich"}, {"eu-north-1", L"Stockholm"},
        {"eu-south-1", L"Milan"}, {"eu-south-2", L"Spain"},
        {"eu-west-1", L"Ireland"}, {"eu-west-2", L"London"},
        {"eu-west-3", L"Paris"}, {"il-central-1", L"Tel Aviv"},
        {"me-central-1", L"UAE"}, {"me-south-1", L"Bahrain"},
        {"mx-central-1", L"Mexico Central"}, {"sa-east-1", L"Sao Paulo"},
        {"us-east-1", L"Northern Virginia"}, {"us-east-2", L"Ohio"},
        {"us-west-1", L"Northern California"}, {"us-west-2", L"Oregon"},
        {"us-gov-east-1", L"US Gov East"}, {"us-gov-west-1", L"US Gov West"},
        {"GLOBAL", L"Global"}
    };
    const auto found = names.find(code);
    return found == names.end() ? Utf8ToWide(code) : std::wstring(found->second);
}

void RequireUniqueKeys(yyjson_val* object) {
    if (!yyjson_is_obj(object) || yyjson_obj_size(object) > 32) Invalid("Invalid or oversized catalog object.");
    std::unordered_set<std::string_view> keys;
    yyjson_obj_iter iter = yyjson_obj_iter_with(object);
    while (auto* key = yyjson_obj_iter_next(&iter)) {
        const std::string_view text(yyjson_get_str(key), yyjson_get_len(key));
        if (!keys.insert(text).second) Invalid("Duplicate JSON object key.");
    }
}

std::string_view RequiredString(yyjson_val* object, const char* key) {
    auto* value = yyjson_obj_get(object, key);
    if (!yyjson_is_str(value)) Invalid("Missing or invalid catalog string field.");
    const std::string_view text(yyjson_get_str(value), yyjson_get_len(value));
    if (text.empty() || text.size() > 128 || text.find('\0') != std::string_view::npos)
        Invalid("Empty, oversized, or invalid catalog string field.");
    return text;
}

bool DateValid(std::string_view date) {
    if (date.size() != 19) return false;
    for (std::size_t i = 0; i < date.size(); ++i) {
        const bool separator = i == 4 || i == 7 || i == 10 || i == 13 || i == 16;
        if (separator ? date[i] != '-' : (date[i] < '0' || date[i] > '9')) return false;
    }
    const auto number = [&](std::size_t start, std::size_t count) {
        int result = 0;
        for (std::size_t i = start; i < start + count; ++i) result = result * 10 + date[i] - '0';
        return result;
    };
    const int year = number(0, 4), month = number(5, 2), day = number(8, 2);
    if (year < 2000 || month < 1 || month > 12 || day < 1) return false;
    constexpr int days[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > days[month - 1] + (month == 2 && leap ? 1 : 0)) return false;
    return number(11, 2) < 24 && number(14, 2) < 60 && number(17, 2) < 60;
}

void CheckNoOverlap(const std::vector<Prefix>& allowed, const std::vector<Prefix>& blocked) {
    std::size_t a = 0, b = 0;
    while (a < allowed.size() && b < blocked.size()) {
        if (Contains(allowed[a], blocked[b]) || Contains(blocked[b], allowed[a]))
            Invalid("Selected and blocked regions have overlapping IP ranges. The catalog cannot safely implement this selection.");
        if (PrefixLess(allowed[a], blocked[b])) ++a;
        else ++b;
    }
}
} // namespace

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    if (text.size() > INT_MAX) Invalid("UTF-8 string is too long.");
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) Invalid("Invalid UTF-8 string.");
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), count) != count)
        Invalid("UTF-8 conversion failed.");
    return output;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    if (text.size() > INT_MAX) Invalid("Unicode string is too long.");
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) Invalid("Invalid Unicode string.");
    std::string output(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), count, nullptr, nullptr) != count)
        Invalid("Unicode conversion failed.");
    return output;
}

std::optional<Prefix> ParsePrefix(std::string_view text) {
    if (text.empty() || text.size() > 128) return std::nullopt;
    const auto slash = text.find('/');
    if (slash == std::string_view::npos || slash == 0 || text.find('/', slash + 1) != std::string_view::npos)
        return std::nullopt;
    const auto addressText = text.substr(0, slash);
    const auto lengthText = text.substr(slash + 1);
    if (lengthText.empty() || lengthText.size() > 3 || (lengthText.size() > 1 && lengthText[0] == '0')) return std::nullopt;
    if (!std::all_of(lengthText.begin(), lengthText.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    unsigned length = 0;
    const auto result = std::from_chars(lengthText.data(), lengthText.data() + lengthText.size(), length);
    if (result.ec != std::errc{} || result.ptr != lengthText.data() + lengthText.size()) return std::nullopt;
    Prefix prefix;
    prefix.ipv6 = addressText.find(':') != std::string_view::npos;
    if (length > (prefix.ipv6 ? 128U : 32U)) return std::nullopt;
    if (!std::all_of(addressText.begin(), addressText.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '.' || c == ':';
    })) return std::nullopt;
    // Reject historical octal/short IPv4 forms explicitly, independent of platform parsing.
    if (!prefix.ipv6) {
        std::size_t start = 0;
        for (int part = 0; part < 4; ++part) {
            const auto end = addressText.find('.', start);
            if ((part == 3) != (end == std::string_view::npos)) return std::nullopt;
            const auto component = addressText.substr(start, end == std::string_view::npos ? end : end - start);
            if (component.empty() || component.size() > 3 || (component.size() > 1 && component.front() == '0')) return std::nullopt;
            if (!std::all_of(component.begin(), component.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
            start = end + 1;
        }
    }
    const std::string terminated(addressText);
    if (InetPtonA(prefix.ipv6 ? AF_INET6 : AF_INET, terminated.c_str(), prefix.address.data()) != 1) return std::nullopt;
    prefix.length = static_cast<std::uint8_t>(length);
    return Normalize(prefix);
}

std::string PrefixText(const Prefix& value) {
    const auto prefix = Normalize(value);
    char address[INET6_ADDRSTRLEN]{};
    if (!InetNtopA(prefix.ipv6 ? AF_INET6 : AF_INET, prefix.address.data(), address, sizeof(address)))
        throw std::runtime_error("Cannot format IP address.");
    return std::string(address) + "/" + std::to_string(prefix.length);
}

std::vector<Prefix> CompactPrefixes(std::vector<Prefix> prefixes) {
    for (auto& prefix : prefixes) prefix = Normalize(prefix);
    std::sort(prefixes.begin(), prefixes.end(), PrefixLess);
    std::vector<Prefix> output;
    output.reserve(prefixes.size());
    for (auto prefix : prefixes) {
        if (!output.empty() && Contains(output.back(), prefix)) continue;
        while (!output.empty() && prefix.length && output.back().ipv6 == prefix.ipv6 && output.back().length == prefix.length) {
            Prefix parent = prefix;
            --parent.length;
            parent = Normalize(parent);
            if (!Contains(parent, output.back())) break;
            output.pop_back();
            prefix = parent;
        }
        if (output.empty() || !Contains(output.back(), prefix)) output.push_back(prefix);
    }
    return output;
}

Catalog ParseCatalog(std::string_view json) {
    if (json.empty() || json.size() > kMaximumJsonBytes) Invalid("Catalog is empty or exceeds the 32 MiB limit.");
    yyjson_read_err error{};
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(
        yyjson_read_opts(const_cast<char*>(json.data()), json.size(), YYJSON_READ_NOFLAG, nullptr, &error), yyjson_doc_free);
    if (!doc) throw std::invalid_argument("Invalid catalog JSON at byte " + std::to_string(error.pos) + ".");
    auto* root = yyjson_doc_get_root(doc.get());
    RequireUniqueKeys(root);
    Catalog catalog;
    const auto created = RequiredString(root, "createDate");
    if (!DateValid(created)) Invalid("Invalid AWS catalog createDate.");
    catalog.created.assign(created);
    auto* ipv4 = yyjson_obj_get(root, "prefixes");
    auto* ipv6 = yyjson_obj_get(root, "ipv6_prefixes");
    if (!yyjson_is_arr(ipv4) || (ipv6 && !yyjson_is_arr(ipv6))) Invalid("Missing or invalid catalog prefix arrays.");
    if (yyjson_arr_size(ipv4) + (ipv6 ? yyjson_arr_size(ipv6) : 0) > kMaximumEntries)
        Invalid("Catalog has too many prefixes.");
    std::map<std::string, Region> regions;
    const auto readArray = [&](yyjson_val* array, bool family6) {
        std::size_t index = 0, maximum = 0;
        yyjson_val* entry = nullptr;
        yyjson_arr_foreach(array, index, maximum, entry) {
            RequireUniqueKeys(entry);
            const auto service = RequiredString(entry, "service");
            const auto code = RequiredString(entry, "region");
            const auto network = RequiredString(entry, family6 ? "ipv6_prefix" : "ip_prefix");
            if (service != "EC2") continue;
            if (!RegionCodeValid(code)) Invalid("Invalid EC2 region code.");
            const auto prefix = ParsePrefix(network);
            if (!prefix || prefix->ipv6 != family6) Invalid("Invalid EC2 IP prefix.");
            auto [position, inserted] = regions.try_emplace(std::string(code));
            auto& region = position->second;
            if (inserted) {
                if (regions.size() > kMaximumRegions) Invalid("Catalog has too many regions.");
                region.code = code;
                region.name = RegionName(code);
            }
            region.prefixes.push_back(*prefix);
            if (family6) ++catalog.ipv6Count;
            else ++catalog.ipv4Count;
        }
    };
    readArray(ipv4, false);
    if (ipv6) readArray(ipv6, true);
    if (regions.empty()) Invalid("Catalog contains no EC2 ranges.");
    catalog.regions.reserve(regions.size());
    for (auto& [code, region] : regions) catalog.regions.push_back(std::move(region));
    return catalog;
}

std::vector<RuleSpec> BuildRules(const Catalog& catalog, const std::vector<std::string>& allowedRegions,
    std::wstring_view application, std::wstring_view generation) {
    if (application.empty() || application.size() > 32767 || std::any_of(application.begin(), application.end(), [](wchar_t c) { return c < 32 || c == 127; }))
        Invalid("A valid game application path is required.");
    if (generation.empty() || generation.size() > 64 || !std::all_of(generation.begin(), generation.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-';
    })) Invalid("A valid rule generation is required.");
    if (allowedRegions.empty()) Invalid("Select at least one region.");
    if (catalog.regions.empty() || catalog.regions.size() > kMaximumRegions) Invalid("Invalid or empty region catalog.");
    std::unordered_set<std::string> known;
    for (const auto& region : catalog.regions) {
        if (!RegionCodeValid(region.code) || region.prefixes.empty() || !known.insert(region.code).second)
            Invalid("Invalid or duplicate catalog region.");
    }
    std::unordered_set<std::string> selected;
    for (const auto& code : allowedRegions) {
        if (!known.contains(code)) Invalid("Selected region does not exist in the catalog.");
        selected.insert(code);
    }
    std::vector<Prefix> allowed, blocked;
    std::size_t total = 0;
    for (const auto& region : catalog.regions) {
        if (region.prefixes.size() > kMaximumEntries - total) Invalid("Catalog has too many prefixes.");
        total += region.prefixes.size();
        auto& destination = selected.contains(region.code) ? allowed : blocked;
        destination.insert(destination.end(), region.prefixes.begin(), region.prefixes.end());
    }
    if (blocked.empty()) return {};
    allowed = CompactPrefixes(std::move(allowed));
    blocked = CompactPrefixes(std::move(blocked));
    CheckNoOverlap(allowed, blocked);
    std::vector<RuleSpec> rules;
    std::wstring addresses;
    addresses.reserve(kMaximumAddressCharacters);
    std::size_t addressCount = 0;
    const auto flush = [&] {
        if (addresses.empty()) return;
        RuleSpec rule;
        rule.name = std::wstring(kRulePrefix) + std::wstring(generation) + L"." + std::to_wstring(rules.size());
        rule.application = application;
        rule.remoteAddresses = std::move(addresses);
        rules.push_back(std::move(rule));
        addresses.clear();
        addresses.reserve(kMaximumAddressCharacters);
        addressCount = 0;
    };
    for (const auto& prefix : blocked) {
        const auto text = Utf8ToWide(PrefixText(prefix));
        if (addressCount == kMaximumAddressesPerRule || addresses.size() + text.size() + (addresses.empty() ? 0 : 1) > kMaximumAddressCharacters)
            flush();
        if (!addresses.empty()) addresses += L',';
        addresses += text;
        ++addressCount;
    }
    flush();
    return rules;
}
} // namespace regionlock
