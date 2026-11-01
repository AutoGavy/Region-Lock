// Copyright (C) 2026 AutoGavy
// SPDX-License-Identifier: AGPL-3.0-only
// See LICENSE in the project root for the full license text.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "Platform.hpp"
#include "PlatformTransaction.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace regionlock;

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F> void MustThrow(F&& action, const char* message) {
    bool threw{};
    try { action(); } catch (const std::exception&) { threw = true; }
    Require(threw, message);
}

std::string Utf16File(std::wstring_view text) {
    std::string bytes("\xff\xfe", 2);
    bytes.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
    return bytes;
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        wchar_t buffer[32768]{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
        Require(length && length < std::size(buffer), "GetTempPath failed");
        const auto parent = std::filesystem::canonical(buffer);
        path = parent / (L"RegionLock-PlatformTests-" + NewGeneration());
        Require(path.parent_path() == parent, "Temporary directory escaped test parent");
        Require(std::filesystem::create_directory(path), "Create temporary directory failed");
    }
    ~TemporaryDirectory() {
        // Tests create flat files and one empty directory only. Avoid recursive
        // deletion and never follow a directory or link created by another app.
        std::error_code error;
        for (std::filesystem::directory_iterator it(path, error), end; !error && it != end; it.increment(error)) {
            std::filesystem::remove(it->path(), error);
        }
        std::filesystem::remove(path, error);
    }
};

void FileAndSettingsTests(const std::filesystem::path& directory) {
    const Paths paths{directory, directory / L"settings.ini", directory / L"catalog.json"};
    const auto initial = LoadSettings(paths);
    Require(initial.gamePath.empty() && initial.allowedRegions.empty(), "Missing settings must start with no selected regions and no game path");

    WriteFileAtomic(paths.catalogFile, "old catalog");
    const std::string replacement(1024 * 1024, 'x');
    WriteFileAtomic(paths.catalogFile, replacement);
    Require(ReadFile(paths.catalogFile) == replacement, "Atomic replacement did not preserve all bytes");
    MustThrow([&] { ReadFile(paths.catalogFile, 128); }, "Read size limit was not enforced");
    MustThrow([&] { WriteFileAtomic(paths.catalogFile, std::string(16 * 1024 * 1024 + 1, 'z')); }, "Write size limit was not enforced");
    Require(ReadFile(paths.catalogFile) == replacement, "Rejected write replaced the previous file");
    MustThrow([&] { ReadFile(directory / L"missing.dat"); }, "Missing file should report an error");

    const auto game = directory / L"游戏.EXE";
    WriteFileAtomic(game, "MZ test fixture; this file is never executed");
    const auto canonical = ValidateGameExecutable(game.wstring());
    Require(canonical == std::filesystem::canonical(game).wstring(), "Executable path was not canonicalized");
    MustThrow([&] { ValidateGameExecutable(L"relative.exe"); }, "Relative game path was accepted");
    MustThrow([&] { ValidateGameExecutable((directory / L"missing.exe").wstring()); }, "Missing game executable was accepted");
    MustThrow([&] { ValidateGameExecutable(paths.catalogFile.wstring()); }, "Non-executable extension was accepted");
    MustThrow([&] { ValidateGameExecutable((directory / L"*.exe").wstring()); }, "Wildcard game path was accepted");
    MustThrow([&] { ValidateGameExecutable(game.wstring() + L"\n"); }, "Control character in game path was accepted");
    const auto fakeDirectory = directory / L"directory.exe";
    std::filesystem::create_directory(fakeDirectory);
    MustThrow([&] { ValidateGameExecutable(fakeDirectory.wstring()); }, "Directory with executable extension was accepted");
    MustThrow([&] { WriteFileAtomic(fakeDirectory, "cannot replace a directory"); }, "Filesystem replacement failure was not reported");
    Require(std::filesystem::is_directory(fakeDirectory), "Failed atomic replacement changed the original destination");

    Settings selected{canonical, {"ap-northeast-1", "ap-southeast-1", "GLOBAL"}};
    SaveSettings(paths, selected);
    const auto goodBytes = ReadFile(paths.settingsFile);
    Require(goodBytes.size() >= 2 && static_cast<unsigned char>(goodBytes[0]) == 0xff &&
        static_cast<unsigned char>(goodBytes[1]) == 0xfe, "Settings are not UTF-16 LE with BOM");
    const auto loaded = LoadSettings(paths);
    Require(loaded.gamePath == canonical && loaded.allowedRegions == selected.allowedRegions, "Settings round trip lost Unicode path or region selection");

    auto invalid = selected;
    invalid.gamePath = (directory / L"missing.exe").wstring();
    MustThrow([&] { SaveSettings(paths, invalid); }, "Invalid executable path should fail before saving");
    Require(ReadFile(paths.settingsFile) == goodBytes, "Invalid game path destroyed saved settings");
    invalid = selected;
    invalid.allowedRegions.push_back("AP-NORTHEAST-1");
    MustThrow([&] { SaveSettings(paths, invalid); }, "Invalid region code was persisted");
    Require(ReadFile(paths.settingsFile) == goodBytes, "Invalid region destroyed saved settings");
    invalid = selected;
    invalid.allowedRegions.push_back(invalid.allowedRegions.front());
    MustThrow([&] { SaveSettings(paths, invalid); }, "Duplicate regions were persisted");
    Require(ReadFile(paths.settingsFile) == goodBytes, "Duplicate region save destroyed settings");

    const std::string malformed[] = {
        "not UTF-16",
        std::string("\xff\xfe\x41", 3),
        std::string("\xff\xfe", 2),
        Utf16File(L"[Other]\r\ngamePath=\r\nallowedRegions=ap-northeast-1\r\n"),
        Utf16File(L"[RegionLock]\r\ngamePath=\r\n"),
        Utf16File(L"[RegionLock]\r\ngamePath=\r\nallowedRegions=ap-northeast-1,\r\n"),
        Utf16File(L"[RegionLock]\r\ngamePath=\r\nallowedRegions=ap-northeast-1,ap-northeast-1\r\n"),
        Utf16File(L"[RegionLock]\r\ngamePath=\r\nallowedRegions=\r\nallowedRegions=ap-northeast-1\r\n"),
        Utf16File(L"[RegionLock]\r\ngamePath=\r\nallowedRegions=\r\n[RegionLock]\r\n"),
        Utf16File(std::wstring(L"[RegionLock]\r\ngamePath=") + L'\0' + L"\r\nallowedRegions=\r\n")
    };
    for (const auto& bytes : malformed) {
        WriteFileAtomic(paths.settingsFile, bytes);
        MustThrow([&] { LoadSettings(paths); }, "Corrupt or incomplete settings silently became defaults");
        Require(ReadFile(paths.settingsFile) == bytes, "Loading invalid settings modified the original file");
    }

    // Obsolete/unknown keys remain readable without reviving keep-on-exit.
    WriteFileAtomic(paths.settingsFile, Utf16File(L"[RegionLock]\r\ngamePath=\r\nallowedRegions=ap-northeast-1\r\nkeepRulesOnExit=1\r\nfutureKey=yes\r\n"));
    Require(LoadSettings(paths).allowedRegions == std::vector<std::string>{"ap-northeast-1"}, "Unknown settings keys broke compatibility");
    SaveSettings(paths, Settings{});
    Require(LoadSettings(paths).gamePath.empty() && LoadSettings(paths).allowedRegions.empty(), "Cleared settings did not persist an unlocked selection");

    std::size_t temporaryCount{};
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == L".tmp") ++temporaryCount;
    }
    Require(temporaryCount == 0, "Atomic file writes left temporary files behind");
}

void CancellationAndIdentifiers() {
    const auto one = NewGeneration();
    const auto two = NewGeneration();
    Require(one.size() == 36 && two.size() == 36 && one != two, "Generation identifiers are not unique GUIDs");
    Require(one.find_first_not_of(L"0123456789abcdefABCDEF-") == std::wstring::npos, "Generation identifier contains unsafe rule name characters");
    std::stop_source cancellation;
    cancellation.request_stop();
    const auto start = std::chrono::steady_clock::now();
    MustThrow([&] { DownloadCatalog(cancellation.get_token()); }, "Cancelled catalog download did not abort");
    const auto probe = ProbeRegion("ap-northeast-1", cancellation.get_token());
    Require(probe.received == 0 && probe.total == 5 && !probe.error.empty(), "Cancelled beacon probe reported success");
    Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "Pre-cancelled operations performed blocking work");
    const auto invalid = ProbeRegion("bad.region/unsafe", {});
    Require(!invalid.error.empty() && invalid.received == 0, "Invalid beacon region performed a successful probe");
    const auto global = ProbeRegion("GLOBAL", {});
    Require(!global.error.empty() && global.received == 0, "GLOBAL ranges must not be probed as a geographic region");
}

struct MockRules {
    std::vector<bool> old{true, true, true};
    std::vector<bool> added{false, false, false, false};
    int failAdd{-1}, failRemove{-1}, failRestore{-1}, failCleanup{-1};
    bool unlocking{};

    std::string Run() {
        try {
            detail::ReplaceRulesTransaction(unlocking ? 0 : added.size(), old.size(),
                [&](std::size_t i) {
                    Require(std::all_of(old.begin(), old.end(), [](bool value) { return value; }), "Old rules changed before full new generation was installed");
                    if (static_cast<int>(i) == failAdd) throw std::runtime_error("injected install failure");
                    Require(!added[i], "New rule installed twice");
                    added[i] = true;
                },
                [&](std::size_t i) {
                    if (static_cast<int>(i) == failCleanup) throw std::runtime_error("injected cleanup failure");
                    Require(added[i], "Rollback tried to remove a new rule that was never added");
                    added[i] = false;
                },
                [&](std::size_t i) {
                    Require(unlocking || std::all_of(added.begin(), added.end(), [](bool value) { return value; }), "Previous rules removed before new generation was complete");
                    if (static_cast<int>(i) == failRemove) throw std::runtime_error("injected old removal failure");
                    Require(old[i], "Old rule removed twice");
                    old[i] = false;
                },
                [&](std::size_t i) {
                    if (static_cast<int>(i) == failRestore) throw std::runtime_error("injected restore failure");
                    Require(!old[i], "Rollback restored an old rule that was never removed");
                    old[i] = true;
                });
        } catch (const std::exception& error) { return error.what(); }
        return {};
    }
    void RequireOriginal() const {
        Require(std::all_of(old.begin(), old.end(), [](bool value) { return value; }), "Rollback lost an old rule");
        Require(std::none_of(added.begin(), added.end(), [](bool value) { return value; }), "Rollback left a new rule installed");
    }
};

void TransactionTests() {
    MockRules success;
    Require(success.Run().empty(), "Successful replacement failed");
    Require(std::none_of(success.old.begin(), success.old.end(), [](bool value) { return value; }) &&
        std::all_of(success.added.begin(), success.added.end(), [](bool value) { return value; }), "Successful replacement did not commit exactly new generation");
    for (int index = 0; index < 4; ++index) {
        MockRules mock;
        mock.failAdd = index;
        const auto error = mock.Run();
        Require(error.find("injected install failure") != std::string::npos && error.find("ROLLBACK INCOMPLETE") == std::string::npos, "Install failure did not report complete rollback");
        mock.RequireOriginal();
    }
    for (int index = 0; index < 3; ++index) {
        MockRules mock;
        mock.failRemove = index;
        const auto error = mock.Run();
        Require(error.find("injected old removal failure") != std::string::npos && error.find("ROLLBACK INCOMPLETE") == std::string::npos, "Old removal failure did not report complete rollback");
        mock.RequireOriginal();
        mock = MockRules{};
        mock.unlocking = true;
        mock.failRemove = index;
        Require(mock.Run().find("injected old removal failure") != std::string::npos, "Unlock failure was not reported");
        mock.RequireOriginal();
    }
    MockRules unlock;
    unlock.unlocking = true;
    Require(unlock.Run().empty() && std::none_of(unlock.old.begin(), unlock.old.end(), [](bool value) { return value; }), "Successful unlock left owned rules installed");
    MockRules rollbackFailure;
    rollbackFailure.failRemove = 2;
    rollbackFailure.failRestore = 0;
    rollbackFailure.failCleanup = 1;
    const auto error = rollbackFailure.Run();
    Require(error.find("ROLLBACK INCOMPLETE") != std::string::npos &&
        error.find("injected old removal failure") != std::string::npos &&
        error.find("injected restore failure") != std::string::npos &&
        error.find("injected cleanup failure") != std::string::npos, "Rollback failures were hidden or original error was lost");
    Require(!rollbackFailure.old[0] && rollbackFailure.old[1] && rollbackFailure.old[2], "Restore rollback stopped after first failure");
    Require(!rollbackFailure.added[0] && rollbackFailure.added[1] && !rollbackFailure.added[2] && !rollbackFailure.added[3], "Cleanup rollback stopped after first failure");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        bool network{};
        std::filesystem::path saveCatalog;
        for (int index = 1; index < argc; ++index) {
            const std::wstring_view argument(argv[index]);
            if (argument == L"--network" && !network) network = true;
            else if (argument == L"--save-catalog" && saveCatalog.empty() && index + 1 < argc) saveCatalog = argv[++index];
            else throw std::runtime_error("Usage: PlatformTests [--network [--save-catalog <absolute-path>]]");
        }
        Require(saveCatalog.empty() || (network && saveCatalog.is_absolute()), "Saving a catalog requires --network and an absolute output path");
        TemporaryDirectory temporary;
        FileAndSettingsTests(temporary.path);
        CancellationAndIdentifiers();
        TransactionTests();
        std::cout << "Platform offline tests passed (no network or firewall changes).\n";
        if (network) {
            const auto bytes = DownloadCatalog({});
            const auto catalog = ParseCatalog(bytes);
            Require(!catalog.regions.empty(), "Live AWS catalog contained no regions");
            std::cout << "HTTPS AWS catalog: " << bytes.size() << " bytes; " << catalog.regions.size()
                << " regions; EC2 prefixes IPv4=" << catalog.ipv4Count << ", IPv6=" << catalog.ipv6Count
                << "; created=" << catalog.created << "\n";
            if (!saveCatalog.empty()) {
                WriteFileAtomic(saveCatalog, bytes);
                std::cout << "Validated AWS catalog saved atomically to " << WideToUtf8(saveCatalog.wstring()) << "\n";
            }
            const auto probe = ProbeRegion("ap-northeast-1", {});
            std::cout << "Tokyo GameLift UDP echo: " << probe.received << '/' << probe.total
                << "; median=" << probe.medianMs << " ms";
            if (!probe.error.empty()) std::cout << "; " << WideToUtf8(probe.error);
            std::cout << "\nNetwork check made no firewall or application cache changes.\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Platform test failure: " << error.what() << '\n';
        return 1;
    }
}
