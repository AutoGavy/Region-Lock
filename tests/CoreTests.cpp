// Copyright (C) 2026 AutoGavy
// SPDX-License-Identifier: AGPL-3.0-only
// See LICENSE in the project root for the full license text.

#include "Core.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
using namespace regionlock;
int checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

void Reject(const std::function<void()>& action, const char* message) {
    bool rejected = false;
    try { action(); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, message);
}

Prefix P(std::string_view text) {
    const auto prefix = ParsePrefix(text);
    if (!prefix) throw std::runtime_error("Bad test prefix.");
    return *prefix;
}

std::string Entry(std::string_view prefix, std::string_view region, std::string_view service = "EC2", bool ipv6 = false) {
    return "{\"" + std::string(ipv6 ? "ipv6_prefix" : "ip_prefix") + "\":\"" + std::string(prefix)
        + "\",\"region\":\"" + std::string(region) + "\",\"service\":\"" + std::string(service) + "\"}";
}

std::string Json(std::string_view v4, std::string_view v6 = {}) {
    return "{\"syncToken\":\"1\",\"createDate\":\"2026-11-01-00-00-00\",\"prefixes\":[" + std::string(v4)
        + "],\"ipv6_prefixes\":[" + std::string(v6) + "]}";
}

void PrefixTests() {
    Check(PrefixText(P("192.168.2.251/24")) == "192.168.2.0/24", "IPv4 host bits normalize");
    Check(PrefixText(P("2001:db8:abcd:ffff::1/48")) == "2001:db8:abcd::/48", "IPv6 host bits normalize");
    Check(PrefixText(P("255.255.255.255/0")) == "0.0.0.0/0", "IPv4 zero route normalizes");
    Check(PrefixText(P("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff/0")) == "::/0", "IPv6 zero route normalizes");
    Check(P("2001:db8::1/128").length == 128, "IPv6 host prefix");
    Check(P("255.255.255.255/32").length == 32, "IPv4 host prefix");
    for (const auto* text : {"", "1.2.3.4", "1.2.3.4/", "1.2.3.4/33", "1.2.3.4/-1", "1.2.3.4/+1", "1.2.3.4/01",
        "1.2.3/24", "1.2.3.256/24", "01.2.3.4/24", "127.1/8", "0x7f000001/8", "1.2.3.4 /24", "1.2.3.4/24 ",
        "1.2.3.4/24/8", "2001:db8::/129", "2001::db8::/64", "fe80::1%5/64", "[::1]/128", "::1/000"}) {
        Check(!ParsePrefix(text), "Malformed prefix accepted");
    }
    const std::string nul("1.2.3.4\0/24", 11);
    Check(!ParsePrefix(nul), "Embedded NUL accepted");
    Check(CompactPrefixes({P("10.0.0.0/25"), P("10.0.0.128/25"), P("10.0.0.1/32")}) == std::vector<Prefix>{P("10.0.0.0/24")}, "Sibling and containment merge");
    Check(CompactPrefixes({P("10.0.3.0/24"), P("10.0.0.0/24"), P("10.0.2.0/24"), P("10.0.1.0/24"), P("10.0.1.0/24")})
        == std::vector<Prefix>{P("10.0.0.0/22")}, "Recursive IPv4 merge");
    Check(CompactPrefixes({P("2001:db8::/33"), P("2001:db8:8000::/33")}) == std::vector<Prefix>{P("2001:db8::/32")}, "IPv6 sibling merge");
    Check(CompactPrefixes({P("0.0.0.0/1"), P("128.0.0.0/1"), P("::/1"), P("8000::/1")})
        == std::vector<Prefix>{P("0.0.0.0/0"), P("::/0")}, "Both families merge to independent default routes");
    Check(CompactPrefixes({P("10.0.0.0/25"), P("10.0.1.0/25")}).size() == 2, "Non-siblings must not merge");
    Check(CompactPrefixes({P("0.0.0.0/0"), P("::/0"), P("2001:db8::/32"), P("1.2.3.4/32")}).size() == 2, "Default routes contain only matching families");

    // Exhaustive membership over a tiny domain checks that compaction neither loses nor adds hosts.
    std::vector<Prefix> hosts;
    bool expected[256]{};
    for (unsigned i = 0; i < 256; ++i) {
        expected[i] = i < 64 || (i >= 128 && i < 192) || i % 7 == 0;
        if (expected[i]) hosts.push_back(P("198.51.100." + std::to_string(i) + "/32"));
    }
    const auto compact = CompactPrefixes(hosts);
    for (unsigned i = 0; i < 256; ++i) {
        bool actual = false;
        for (const auto& prefix : compact) {
            const unsigned span = 1U << (32 - prefix.length);
            if (i >= prefix.address[3] && i < prefix.address[3] + span) actual = true;
        }
        Check(actual == expected[i], "Compaction changes address membership");
    }
}

void CatalogTests() {
    const auto mixed = ParseCatalog(Json(Entry("203.0.113.8/24", "ap-northeast-1") + ","
        + Entry("198.51.100.0/24", "future-region-9") + "," + Entry("192.0.2.0/24", "other-1", "EC2_INSTANCE_CONNECT"),
        Entry("2001:db8::1/32", "future-region-9", "EC2", true)));
    Check(mixed.regions.size() == 2 && mixed.ipv4Count == 2 && mixed.ipv6Count == 1, "Exact EC2 service filter or family counts");
    Check(mixed.regions[0].code == "ap-northeast-1" && mixed.regions[0].name == L"Tokyo", "Known English region name");
    Check(mixed.regions[1].name == L"future-region-9" && mixed.regions[1].prefixes.size() == 2, "Future regions stay separate");
    Check(mixed.regions[0].prefixes[0] == P("203.0.113.0/24"), "Catalog normalizes network bits");
    Check(ParseCatalog("{\"createDate\":\"2024-02-29-23-59-59\",\"prefixes\":[" + Entry("10.0.0.0/8", "us-east-1") + "]}").ipv6Count == 0,
        "IPv6 array is optional and leap dates work");
    Check(ParseCatalog(Json("", Entry("2001:db8::/32", "us-east-1", "EC2", true))).ipv6Count == 1, "IPv6-only EC2 data");
    for (const auto& json : {
        std::string(""), std::string("[]"), std::string("null"), std::string("{}"), std::string("{\"createDate\":\"2026-11-01-00-00-00\"}"),
        Json(""), Json(Entry("10.0.0.0/8", "us-east-1", "AMAZON")), Json(Entry("10.0.0.0/33", "us-east-1")),
        Json(Entry("2001:db8::/32", "us-east-1")), Json("", Entry("10.0.0.0/8", "us-east-1", "EC2", true)),
        Json(Entry("10.0.0.0/8", "unknown region")), Json(Entry("10.0.0.0/8", "-region")),
        Json("{\"ip_prefix\":\"10.0.0.0/8\",\"service\":\"EC2\"}"),
        Json("{\"ip_prefix\":\"10.0.0.0/8\",\"region\":\"us-east-1\",\"service\":false}"),
        Json("{\"ip_prefix\":\"10.0.0.0/8\",\"region\":\"us-east-1\",\"service\":\"EC2\",\"service\":\"AMAZON\"}"),
        std::string("{\"createDate\":\"2026-11-01-00-00-00\",\"prefixes\":[],\"ipv6_prefixes\":null}"),
        std::string("{\"createDate\":\"2026-11-01-00-00-00\",\"createDate\":\"2026-11-01-00-00-00\",\"prefixes\":[]}"),
        Json(Entry("10.0.0.0/8", "us-east-1")) + " trailing",
        std::string("{\"createDate\":\"2026-11-01-00-00-00\",\"prefixes\":[],}"),
        std::string("{/*comment*/\"createDate\":\"2026-11-01-00-00-00\",\"prefixes\":[]}")
    }) Reject([&] { (void)ParseCatalog(json); }, "Malformed catalog accepted");
    for (const auto* date : {"2026-02-29-00-00-00", "2026-04-31-00-00-00", "2026-00-01-00-00-00", "2026-11-01-24-00-00", "2026-11-01-00-60-00", "2026-11-01-00-00-60", "2026/11/01 00:00:00"}) {
        auto invalid = Json(Entry("10.0.0.0/8", "us-east-1"));
        invalid.replace(invalid.find("2026-11-01-00-00-00"), 19, date);
        Reject([&] { (void)ParseCatalog(invalid); }, "Invalid catalog date accepted");
    }
    Reject([&] { (void)ParseCatalog(std::string(32 * 1024 * 1024 + 1, ' ')); }, "Oversized catalog accepted");
}

void RuleTests() {
    const auto catalog = ParseCatalog(Json(Entry("192.0.2.0/24", "allowed-1") + "," + Entry("198.51.100.0/25", "blocked-1")
        + "," + Entry("198.51.100.128/25", "blocked-1"), Entry("2001:db8:1::/48", "blocked-1", "EC2", true)));
    const auto rules = BuildRules(catalog, {"allowed-1"}, L"C:\\Games\\bf6.exe", L"test-1");
    Check(rules.size() == 1 && rules[0].name == std::wstring(kRulePrefix) + L"test-1.0", "Deterministic generated rule name");
    Check(rules[0].application == L"C:\\Games\\bf6.exe" && rules[0].firstPort == 21000 && rules[0].lastPort == 21299, "Rules are game scoped with correct remote ports");
    Check(rules[0].remoteAddresses == L"198.51.100.0/24,2001:db8:1::/48", "Only blocked ranges enter compact address list");
    Check(BuildRules(catalog, {"allowed-1", "blocked-1"}, L"x.exe", L"g").empty(), "All selected creates no block rules");
    Check(BuildRules(catalog, {"allowed-1", "allowed-1"}, L"x.exe", L"g").size() == 1, "Repeated selections do not duplicate rules");
    Reject([&] { (void)BuildRules(catalog, {}, L"x.exe", L"g"); }, "No selection must fail");
    Reject([&] { (void)BuildRules(catalog, {"unknown"}, L"x.exe", L"g"); }, "Unknown selection must fail");
    Reject([&] { (void)BuildRules(catalog, {"allowed-1"}, L"", L"g"); }, "Empty application must fail");
    Reject([&] { (void)BuildRules(catalog, {"allowed-1"}, L"bad\n.exe", L"g"); }, "Control characters in application must fail");
    Reject([&] { (void)BuildRules(catalog, {"allowed-1"}, L"x.exe", L""); }, "Empty generation must fail");
    Reject([&] { (void)BuildRules(catalog, {"allowed-1"}, L"x.exe", L"{bad}"); }, "Unsafe generation must fail");
    const auto overlap = ParseCatalog(Json(Entry("10.0.0.0/16", "allowed-1") + "," + Entry("10.0.1.0/24", "blocked-1")));
    Reject([&] { (void)BuildRules(overlap, {"allowed-1"}, L"x.exe", L"g"); }, "Blocked subrange of allowed prefix must fail closed");
    Reject([&] { (void)BuildRules(overlap, {"blocked-1"}, L"x.exe", L"g"); }, "Allowed subrange of blocked prefix must fail closed");
    const auto overlap6 = ParseCatalog(Json(Entry("192.0.2.0/24", "allowed-1"), Entry("2001:db8::/32", "allowed-1", "EC2", true)
        + "," + Entry("2001:db8:1::/48", "blocked-1", "EC2", true)));
    Reject([&] { (void)BuildRules(overlap6, {"allowed-1"}, L"x.exe", L"g"); }, "IPv6 region overlap must fail closed");

    Catalog large;
    large.created = "2026-11-01-00-00-00";
    large.regions.push_back({"allowed-1", L"Allowed", {P("192.0.2.0/24")}});
    large.regions.push_back({"blocked-1", L"Blocked", {}});
    for (unsigned i = 0; i < 1001; ++i)
        large.regions[1].prefixes.push_back(P("10." + std::to_string(i / 128) + "." + std::to_string((i % 128) * 2) + ".1/32"));
    const auto chunks = BuildRules(large, {"allowed-1"}, L"x.exe", L"chunks");
    Check(chunks.size() == 3, "Address lists split into bounded chunks");
    std::size_t count = 0;
    for (const auto& rule : chunks) {
        const auto entries = static_cast<std::size_t>(std::count(rule.remoteAddresses.begin(), rule.remoteAddresses.end(), L',')) + 1;
        count += entries;
        Check(entries <= 400 && rule.remoteAddresses.size() <= 16000, "Rule size limits exceeded");
        Check(rule.remoteAddresses.back() != L',' && rule.remoteAddresses.front() != L',', "Malformed comma list");
    }
    Check(count == 1001, "Chunking loses addresses");

    // Long IPv6 host literals exercise the character limit before the count limit.
    large.regions[1].prefixes.clear();
    for (unsigned i = 0; i < 501; ++i) {
        auto prefix = P("2001:db8:abcd:1234:5678:9abc:def0:1000/128");
        prefix.address[14] = static_cast<std::uint8_t>(i >> 7);
        prefix.address[15] = static_cast<std::uint8_t>((i & 127) * 2);
        large.regions[1].prefixes.push_back(prefix);
    }
    const auto longChunks = BuildRules(large, {"allowed-1"}, L"x.exe", L"long");
    Check(longChunks.size() == 2, "Long IPv6 chunks split");
    for (const auto& rule : longChunks) Check(rule.remoteAddresses.size() <= 16000, "Long IPv6 character limit exceeded");
}

void UtfTests() {
    const std::string text = "战地 6 / 日本 / \xF0\x9F\x8E\xAE";
    Check(WideToUtf8(Utf8ToWide(text)) == text, "Unicode round trip");
    Check(Utf8ToWide({}).empty() && WideToUtf8({}).empty(), "Empty Unicode round trip");
    Reject([] { (void)Utf8ToWide(std::string("\xC0\xAF", 2)); }, "Overlong UTF-8 accepted");
    Reject([] { (void)Utf8ToWide(std::string("\xED\xA0\x80", 3)); }, "UTF-8 surrogate accepted");
    Reject([] { (void)WideToUtf8(std::wstring(1, static_cast<wchar_t>(0xD800))); }, "Unpaired UTF-16 surrogate accepted");
}

void Benchmark(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open benchmark catalog.");
    const std::string json((std::istreambuf_iterator<char>(file)), {});
    const auto start = std::chrono::steady_clock::now();
    const auto catalog = ParseCatalog(json);
    const auto parsed = std::chrono::steady_clock::now();
    const auto selected = std::find_if(catalog.regions.begin(), catalog.regions.end(), [](const Region& region) { return region.code == "ap-northeast-1"; });
    const std::string code = selected == catalog.regions.end() ? catalog.regions.front().code : selected->code;
    const auto rules = BuildRules(catalog, {code}, L"C:\\offline-benchmark\\bf6.exe", L"benchmark");
    const auto built = std::chrono::steady_clock::now();
    std::size_t entries = 0;
    for (const auto& rule : rules) entries += static_cast<std::size_t>(std::count(rule.remoteAddresses.begin(), rule.remoteAddresses.end(), L',')) + 1;
    std::cout << "benchmark bytes=" << json.size() << " regions=" << catalog.regions.size()
        << " ipv4=" << catalog.ipv4Count << " ipv6=" << catalog.ipv6Count << " selected=" << code
        << " compact_blocked=" << entries << " rules=" << rules.size()
        << " parse_ms=" << std::chrono::duration<double, std::milli>(parsed - start).count()
        << " build_ms=" << std::chrono::duration<double, std::milli>(built - parsed).count() << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        PrefixTests();
        CatalogTests();
        RuleTests();
        UtfTests();
        std::cout << "PASS: " << checks << " offline core checks (no firewall changes).\n";
        if (argc > 2) throw std::invalid_argument("Usage: RegionLockCoreTests [AWS-ip-ranges.json]");
        if (argc == 2) Benchmark(std::filesystem::path(argv[1]));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
