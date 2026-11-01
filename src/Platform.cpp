// Copyright (C) 2026 AutoGavy
// SPDX-License-Identifier: AGPL-3.0-only
// See LICENSE in the project root for the full license text.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <netfw.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include "Platform.hpp"
#include "PlatformTransaction.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

namespace regionlock {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxCatalogBytes = 16 * 1024 * 1024;
std::mutex firewallMutationMutex;

std::string WindowsMessage(DWORD code) {
    wchar_t* message{};
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring value = size ? std::wstring(message, size) : L"Unknown Windows error";
    if (message) LocalFree(message);
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    return WideToUtf8(value) + " (" + std::to_string(code) + ")";
}

[[noreturn]] void FailWin32(std::string_view operation, DWORD code = GetLastError()) {
    throw std::runtime_error(std::string(operation) + ": " + WindowsMessage(code));
}

void CheckHr(HRESULT result, std::string_view operation) {
    if (FAILED(result)) FailWin32(operation, static_cast<DWORD>(result));
}

template<class T> class ComPtr {
    T* ptr_{};
public:
    ComPtr() = default;
    ~ComPtr() { if (ptr_) ptr_->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            if (ptr_) ptr_->Release();
            ptr_ = std::exchange(other.ptr_, nullptr);
        }
        return *this;
    }
    T* operator->() const { return ptr_; }
    T* get() const { return ptr_; }
    T** put() { return &ptr_; }
};

struct ComApartment {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ComApartment() {
        if (result != RPC_E_CHANGED_MODE) CheckHr(result, "Initialize COM");
    }
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

struct Bstr {
    BSTR value{};
    Bstr() = default;
    explicit Bstr(std::wstring_view text) {
        value = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        if (!value) throw std::bad_alloc();
    }
    ~Bstr() { SysFreeString(value); }
    Bstr(const Bstr&) = delete;
    Bstr(Bstr&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    BSTR* put() { return &value; }
    std::wstring str() const { return value ? std::wstring(value, SysStringLen(value)) : std::wstring(); }
};

struct Variant {
    VARIANT value;
    Variant() { VariantInit(&value); }
    ~Variant() { VariantClear(&value); }
};

class FileHandle {
    HANDLE value_{INVALID_HANDLE_VALUE};
public:
    explicit FileHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~FileHandle() { close(); }
    FileHandle(const FileHandle&) = delete;
    HANDLE get() const { return value_; }
    bool valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
    void close() { if (valid()) CloseHandle(std::exchange(value_, INVALID_HANDLE_VALUE)); }
};

struct HttpHandle {
    HINTERNET value{};
    explicit HttpHandle(HINTERNET handle) : value(handle) {}
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle(const HttpHandle&) = delete;
};

void CheckStop(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Operation cancelled");
}

bool HasControl(std::wstring_view text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t c) { return c < 32 || c == 127; });
}

bool ValidRegionCode(std::string_view code) {
    if (code == "GLOBAL") return true;
    return !code.empty() && code.size() <= 64 && std::all_of(code.begin(), code.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

std::wstring Trim(std::wstring_view value) {
    while (!value.empty() && (value.front() == L' ' || value.front() == L'\t' || value.front() == L'\r')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t' || value.back() == L'\r')) value.remove_suffix(1);
    return std::wstring(value);
}

std::wstring ServiceProblem();

ComPtr<INetFwPolicy2> FirewallPolicy() {
    ComPtr<INetFwPolicy2> policy;
    const HRESULT result = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(INetFwPolicy2), reinterpret_cast<void**>(policy.put()));
    if (FAILED(result)) {
        const auto services = ServiceProblem();
        if (!services.empty()) throw std::runtime_error(WideToUtf8(services) + "Open Windows Firewall policy: " + WindowsMessage(static_cast<DWORD>(result)));
        CheckHr(result, "Open Windows Firewall policy");
    }
    return policy;
}

ComPtr<INetFwRules> FirewallRules(INetFwPolicy2* policy) {
    ComPtr<INetFwRules> rules;
    CheckHr(policy->get_Rules(rules.put()), "Read Windows Firewall rules");
    return rules;
}

std::wstring ReadRuleName(INetFwRule* rule) {
    Bstr name;
    CheckHr(rule->get_Name(name.put()), "Read firewall rule name");
    return name.str();
}

bool IsOwnedRule(INetFwRule* rule, const std::wstring& name) {
    if (!name.starts_with(kRulePrefix)) return false;
    Bstr group;
    CheckHr(rule->get_Grouping(group.put()), "Read firewall rule group");
    return group.str() == kRuleGroup;
}

ComPtr<INetFwRule> NewRule() {
    ComPtr<INetFwRule> rule;
    CheckHr(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(INetFwRule), reinterpret_cast<void**>(rule.put())), "Create firewall rule object");
    return rule;
}

// Materialize every INetFwRule property before changing the policy. Keeping a
// live policy object alone is not a reliable backup after Rules.Remove().
ComPtr<INetFwRule> CloneRule(INetFwRule* source) {
    auto target = NewRule();
    auto copyText = [&](auto get, auto put, const char* label) {
        Bstr value;
        CheckHr((source->*get)(value.put()), label);
        CheckHr((target.get()->*put)(value.value), label);
    };
    copyText(&INetFwRule::get_Name, &INetFwRule::put_Name, "Snapshot rule name");
    copyText(&INetFwRule::get_Description, &INetFwRule::put_Description, "Snapshot rule description");
    copyText(&INetFwRule::get_ApplicationName, &INetFwRule::put_ApplicationName, "Snapshot application path");
    copyText(&INetFwRule::get_ServiceName, &INetFwRule::put_ServiceName, "Snapshot service name");
    long protocol{};
    CheckHr(source->get_Protocol(&protocol), "Snapshot rule protocol");
    CheckHr(target->put_Protocol(protocol), "Snapshot rule protocol");
    if (protocol == NET_FW_IP_PROTOCOL_TCP || protocol == NET_FW_IP_PROTOCOL_UDP) {
        copyText(&INetFwRule::get_LocalPorts, &INetFwRule::put_LocalPorts, "Snapshot local ports");
        copyText(&INetFwRule::get_RemotePorts, &INetFwRule::put_RemotePorts, "Snapshot remote ports");
    }
    copyText(&INetFwRule::get_LocalAddresses, &INetFwRule::put_LocalAddresses, "Snapshot local addresses");
    copyText(&INetFwRule::get_RemoteAddresses, &INetFwRule::put_RemoteAddresses, "Snapshot remote addresses");
    if (protocol == 1 || protocol == 58) {
        copyText(&INetFwRule::get_IcmpTypesAndCodes, &INetFwRule::put_IcmpTypesAndCodes, "Snapshot ICMP settings");
    }
    NET_FW_RULE_DIRECTION direction{};
    CheckHr(source->get_Direction(&direction), "Snapshot rule direction");
    CheckHr(target->put_Direction(direction), "Snapshot rule direction");
    Variant interfaces;
    CheckHr(source->get_Interfaces(&interfaces.value), "Snapshot rule interfaces");
    CheckHr(target->put_Interfaces(interfaces.value), "Snapshot rule interfaces");
    copyText(&INetFwRule::get_InterfaceTypes, &INetFwRule::put_InterfaceTypes, "Snapshot interface types");
    VARIANT_BOOL enabled{}, edge{};
    CheckHr(source->get_Enabled(&enabled), "Snapshot rule enabled state");
    CheckHr(target->put_Enabled(enabled), "Snapshot rule enabled state");
    copyText(&INetFwRule::get_Grouping, &INetFwRule::put_Grouping, "Snapshot rule group");
    long profiles{};
    CheckHr(source->get_Profiles(&profiles), "Snapshot rule profiles");
    CheckHr(target->put_Profiles(profiles), "Snapshot rule profiles");
    CheckHr(source->get_EdgeTraversal(&edge), "Snapshot edge traversal");
    CheckHr(target->put_EdgeTraversal(edge), "Snapshot edge traversal");
    NET_FW_ACTION action{};
    CheckHr(source->get_Action(&action), "Snapshot rule action");
    CheckHr(target->put_Action(action), "Snapshot rule action");
    ComPtr<INetFwRule2> source2, target2;
    const HRESULT supportsRule2 = source->QueryInterface(__uuidof(INetFwRule2), reinterpret_cast<void**>(source2.put()));
    if (supportsRule2 != E_NOINTERFACE) {
        CheckHr(supportsRule2, "Snapshot extended firewall rule");
        CheckHr(target->QueryInterface(__uuidof(INetFwRule2), reinterpret_cast<void**>(target2.put())), "Create extended firewall snapshot");
        long options{};
        CheckHr(source2->get_EdgeTraversalOptions(&options), "Snapshot edge traversal options");
        CheckHr(target2->put_EdgeTraversalOptions(options), "Snapshot edge traversal options");
    }
    ComPtr<INetFwRule3> source3, target3;
    const HRESULT supportsRule3 = source->QueryInterface(__uuidof(INetFwRule3), reinterpret_cast<void**>(source3.put()));
    if (supportsRule3 != E_NOINTERFACE) {
        CheckHr(supportsRule3, "Snapshot extended firewall security");
        CheckHr(target->QueryInterface(__uuidof(INetFwRule3), reinterpret_cast<void**>(target3.put())), "Create extended firewall security snapshot");
        long flags{};
        CheckHr(source3->get_SecureFlags(&flags), "Snapshot firewall security flags");
        CheckHr(target3->put_SecureFlags(flags), "Snapshot firewall security flags");
        auto copySecurityText = [&](auto get, auto put, const char* label) {
            Bstr value;
            CheckHr((source3.get()->*get)(value.put()), label);
            // An unset optional value is represented by a null BSTR. A new
            // rule already has that default; some setters reject null values.
            if (value.value && SysStringLen(value.value)) CheckHr((target3.get()->*put)(value.value), label);
        };
        copySecurityText(&INetFwRule3::get_LocalAppPackageId, &INetFwRule3::put_LocalAppPackageId, "Snapshot application package ID");
        copySecurityText(&INetFwRule3::get_LocalUserOwner, &INetFwRule3::put_LocalUserOwner, "Snapshot local user owner");
        copySecurityText(&INetFwRule3::get_LocalUserAuthorizedList, &INetFwRule3::put_LocalUserAuthorizedList, "Snapshot local authorized users");
        copySecurityText(&INetFwRule3::get_RemoteUserAuthorizedList, &INetFwRule3::put_RemoteUserAuthorizedList, "Snapshot remote authorized users");
        copySecurityText(&INetFwRule3::get_RemoteMachineAuthorizedList, &INetFwRule3::put_RemoteMachineAuthorizedList, "Snapshot authorized remote machines");
    }
    return target;
}

struct RuleBackup {
    std::wstring name;
    ComPtr<INetFwRule> rule;
};

struct IgnoreCase {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};

struct RuleInventory {
    std::vector<RuleBackup> owned;
    std::map<std::wstring, unsigned, IgnoreCase> nameCounts;
};

RuleInventory Inventory(INetFwRules* rules, bool snapshot) {
    ComPtr<IUnknown> unknown;
    CheckHr(rules->get__NewEnum(unknown.put()), "Enumerate firewall rules");
    ComPtr<IEnumVARIANT> enumerator;
    CheckHr(unknown->QueryInterface(__uuidof(IEnumVARIANT), reinterpret_cast<void**>(enumerator.put())), "Enumerate firewall rules");
    RuleInventory inventory;
    for (;;) {
        Variant item;
        ULONG fetched{};
        const HRESULT next = enumerator->Next(1, &item.value, &fetched);
        CheckHr(next, "Enumerate firewall rules");
        if (!fetched) break;
        IUnknown* object = item.value.vt == VT_DISPATCH ? item.value.pdispVal :
            item.value.vt == VT_UNKNOWN ? item.value.punkVal : nullptr;
        if (!object) continue;
        ComPtr<INetFwRule> rule;
        CheckHr(object->QueryInterface(__uuidof(INetFwRule), reinterpret_cast<void**>(rule.put())), "Inspect firewall rule");
        auto name = ReadRuleName(rule.get());
        ++inventory.nameCounts[name];
        if (IsOwnedRule(rule.get(), name)) {
            inventory.owned.push_back({std::move(name), snapshot ? CloneRule(rule.get()) : ComPtr<INetFwRule>()});
        }
    }
    if (snapshot) {
        for (const auto& entry : inventory.owned) {
            if (inventory.nameCounts.at(entry.name) != 1) {
                throw std::runtime_error("Duplicate firewall rule name prevents safe removal: " + WideToUtf8(entry.name));
            }
        }
    }
    return inventory;
}

std::wstring ServiceProblem() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return {};
    std::wstring problem;
    for (const wchar_t* name : {L"BFE", L"MpsSvc"}) {
        SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
        if (!service) continue;
        SERVICE_STATUS_PROCESS status{};
        DWORD needed{};
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&status), sizeof(status), &needed) && status.dwCurrentState != SERVICE_RUNNING) {
            problem += std::wstring(name) + L" service is not running. ";
        }
        QueryServiceConfigW(service, nullptr, 0, &needed);
        if (needed) {
            std::vector<BYTE> config(needed);
            auto data = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config.data());
            if (QueryServiceConfigW(service, data, needed, &needed) && data->dwStartType == SERVICE_DISABLED) {
                problem += std::wstring(name) + L" service is disabled. ";
            }
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return problem;
}

bool ActiveFirewallEnabled(INetFwPolicy2* policy) {
    long active{};
    CheckHr(policy->get_CurrentProfileTypes(&active), "Read active firewall profiles");
    if (active == 0) return false;
    for (auto profile : {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
        if (!(active & profile)) continue;
        VARIANT_BOOL enabled{};
        CheckHr(policy->get_FirewallEnabled(profile, &enabled), "Read active firewall enabled state");
        if (enabled == VARIANT_FALSE) return false;
    }
    return true;
}

void RequireWritableFirewall(INetFwPolicy2* policy, bool applying) {
    const auto services = ServiceProblem();
    if (!services.empty()) throw std::runtime_error(WideToUtf8(services) + "Enable/start Windows Firewall services in Windows before retrying.");
    NET_FW_MODIFY_STATE state{};
    CheckHr(policy->get_LocalPolicyModifyState(&state), "Read firewall policy modification state");
    if (state != NET_FW_MODIFY_STATE_OK) {
        throw std::runtime_error("Windows policy does not allow local firewall changes (Group Policy or inbound blocking policy).");
    }
    if (applying && !ActiveFirewallEnabled(policy)) {
        throw std::runtime_error("Windows Firewall is disabled on an active network profile. Enable it in Windows before applying region rules.");
    }
}

ComPtr<INetFwRule> MakeRule(const RuleSpec& spec) {
    auto rule = NewRule();
    const auto application = ValidateGameExecutable(spec.application);
    if (!spec.name.starts_with(kRulePrefix) || spec.name.size() > 240 || HasControl(spec.name) || spec.name.size() <= std::size(kRulePrefix)) {
        throw std::runtime_error("Invalid owned firewall rule name");
    }
    if (spec.firstPort != kFirstPort || spec.lastPort != kLastPort || spec.remoteAddresses.empty() || spec.remoteAddresses.size() > 24000) {
        throw std::runtime_error("Invalid firewall rule ports or address list");
    }
    // Only canonical IP CIDRs are accepted, never keywords such as '*' or
    // 'LocalSubnet'. This protects callers that bypass BuildRules().
    std::size_t start{};
    while (start < spec.remoteAddresses.size()) {
        auto end = spec.remoteAddresses.find(L',', start);
        if (end == std::wstring::npos) end = spec.remoteAddresses.size();
        const auto address = spec.remoteAddresses.substr(start, end - start);
        if (!ParsePrefix(WideToUtf8(address))) throw std::runtime_error("Firewall address list contains an invalid IP prefix");
        start = end + 1;
    }
    if (spec.remoteAddresses.back() == L',') throw std::runtime_error("Firewall address list has an empty entry");
    Bstr name(spec.name), app(application), group(kRuleGroup), addresses(spec.remoteAddresses);
    Bstr description(L"Battlefield 6 region selection. Blocks only this game's UDP ports 21000-21299 to unselected AWS EC2 regions.");
    Bstr ports(std::to_wstring(kFirstPort) + L"-" + std::to_wstring(kLastPort));
    Bstr any(L"*"), interfaces(L"All");
    CheckHr(rule->put_Name(name.value), "Set firewall rule name");
    CheckHr(rule->put_Description(description.value), "Set firewall rule description");
    CheckHr(rule->put_ApplicationName(app.value), "Bind firewall rule to game executable");
    CheckHr(rule->put_Protocol(NET_FW_IP_PROTOCOL_UDP), "Set firewall rule protocol");
    CheckHr(rule->put_RemotePorts(ports.value), "Set firewall remote ports");
    CheckHr(rule->put_LocalAddresses(any.value), "Set firewall local addresses");
    CheckHr(rule->put_RemoteAddresses(addresses.value), "Set firewall remote addresses");
    CheckHr(rule->put_Direction(NET_FW_RULE_DIR_OUT), "Set firewall rule direction");
    CheckHr(rule->put_InterfaceTypes(interfaces.value), "Set firewall interface types");
    CheckHr(rule->put_Profiles(NET_FW_PROFILE2_ALL), "Set firewall rule profiles");
    CheckHr(rule->put_Grouping(group.value), "Set firewall rule group");
    CheckHr(rule->put_EdgeTraversal(VARIANT_FALSE), "Set firewall edge traversal");
    CheckHr(rule->put_Action(NET_FW_ACTION_BLOCK), "Set firewall rule action");
    CheckHr(rule->put_Enabled(VARIANT_TRUE), "Enable firewall rule");
    return rule;
}

struct Winsock {
    Winsock() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result) FailWin32("Initialize Winsock", result);
    }
    ~Winsock() { WSACleanup(); }
};

struct Socket {
    SOCKET value{INVALID_SOCKET};
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};

using AddressList = std::unique_ptr<ADDRINFOEXW, decltype(&FreeAddrInfoExW)>;

AddressList ResolveBeacon(std::wstring_view host, std::stop_token stop) {
    CheckStop(stop);
    ADDRINFOEXW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    const std::wstring hostname(host);
    FileHandle completed(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!completed.valid()) FailWin32("Create asynchronous DNS completion event");
    OVERLAPPED overlapped{};
    overlapped.hEvent = completed.get();
    ADDRINFOEXW* resolved{};
    HANDLE query{};
    timeval timeout{3, 0};
    int lookup = GetAddrInfoExW(hostname.c_str(), L"7770", NS_DNS, nullptr, &hints,
        &resolved, &timeout, &overlapped, nullptr, &query);
    bool timedOut{};
    DWORD waitError{};
    if (lookup == WSA_IO_PENDING) {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        for (;;) {
            const DWORD wait = WaitForSingleObject(completed.get(), 50);
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_FAILED) waitError = GetLastError();
            timedOut = Clock::now() >= deadline;
            if (waitError || timedOut || stop.stop_requested()) {
                // The asynchronous operation may complete between the wait
                // and cancellation. Cancel may then report an expired query;
                // either way, drain its completion before stack storage dies.
                if (query) GetAddrInfoExCancel(&query);
                WaitForSingleObject(completed.get(), INFINITE);
                break;
            }
        }
        lookup = GetAddrInfoExOverlappedResult(&overlapped);
    }
    AddressList addresses(resolved, FreeAddrInfoExW);
    CheckStop(stop);
    if (waitError) FailWin32("Wait for asynchronous beacon DNS lookup", waitError);
    if (timedOut) throw std::runtime_error("Beacon DNS lookup timed out after 3 seconds");
    if (lookup) {
        if (lookup == WSAHOST_NOT_FOUND || lookup == WSANO_DATA) {
            throw std::runtime_error("This region's GameLift beacon hostname is unavailable: " + WindowsMessage(lookup));
        }
        FailWin32("Resolve GameLift beacon DNS", lookup);
    }
    if (!addresses) throw std::runtime_error("Beacon DNS lookup returned no addresses");
    return addresses;
}

bool WaitUntil(Clock::time_point until, std::stop_token stop) {
    std::mutex mutex;
    std::unique_lock lock(mutex);
    std::condition_variable_any condition;
    condition.wait_until(lock, stop, until, [] { return false; });
    return !stop.stop_requested();
}
} // namespace

Paths GetPaths() {
    PWSTR local{};
    CheckHr(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local), "Find LocalAppData directory");
    std::filesystem::path directory(local);
    CoTaskMemFree(local);
    directory /= L"RegionLockNative";
    return {directory, directory / L"settings.ini", directory / L"ip-ranges.json"};
}

std::string ReadFile(const std::filesystem::path& path, std::size_t limit) {
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) FailWin32("Open " + WideToUtf8(path.wstring()));
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) FailWin32("Read file size");
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > limit) throw std::runtime_error("File exceeds size limit");
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset{};
    while (offset < bytes.size()) {
        DWORD count{};
        const auto request = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024 * 1024));
        if (!::ReadFile(file.get(), bytes.data() + offset, request, &count, nullptr)) FailWin32("Read file");
        if (!count) throw std::runtime_error("File was truncated while reading");
        offset += count;
    }
    return bytes;
}

void WriteFileAtomic(const std::filesystem::path& path, std::string_view bytes) {
    if (bytes.size() > kMaxCatalogBytes) throw std::runtime_error("Output file exceeds size limit");
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L"." + NewGeneration() + L".tmp";
    FileHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) FailWin32("Create temporary file");
    try {
        std::size_t offset{};
        while (offset < bytes.size()) {
            DWORD count{};
            const auto request = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024 * 1024));
            if (!::WriteFile(file.get(), bytes.data() + offset, request, &count, nullptr)) FailWin32("Write temporary file");
            if (!count) throw std::runtime_error("Writing temporary file made no progress");
            offset += count;
        }
        if (!FlushFileBuffers(file.get())) FailWin32("Flush temporary file");
        file.close();
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) FailWin32("Replace saved file");
    } catch (...) {
        file.close();
        DeleteFileW(temporary.c_str());
        throw;
    }
}

Settings LoadSettings(const Paths& paths) {
    const DWORD attributes = GetFileAttributesW(paths.settingsFile.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
        FailWin32("Read settings attributes", error);
    }
    auto bytes = ReadFile(paths.settingsFile, 64 * 1024);
    if (bytes.size() < 2 || bytes.size() % 2 || static_cast<unsigned char>(bytes[0]) != 0xff || static_cast<unsigned char>(bytes[1]) != 0xfe) {
        throw std::runtime_error("Settings file must be UTF-16 LE with BOM");
    }
    std::wstring content((bytes.size() - 2) / sizeof(wchar_t), L'\0');
    if (!content.empty()) std::memcpy(content.data(), bytes.data() + 2, content.size() * sizeof(wchar_t));
    if (content.find(L'\0') != std::wstring::npos) throw std::runtime_error("Settings file contains a NUL character");
    Settings settings;
    bool section{};
    bool sawSection{}, sawPath{}, sawRegions{};
    for (std::size_t offset = 0; offset < content.size();) {
        auto end = content.find(L'\n', offset);
        if (end == std::wstring::npos) end = content.size();
        auto line = Trim(std::wstring_view(content).substr(offset, end - offset));
        offset = end + 1;
        if (line.empty() || line.front() == L';' || line.front() == L'#') continue;
        if (line.front() == L'[') {
            section = line == L"[RegionLock]";
            if (section) {
                if (sawSection) throw std::runtime_error("Duplicate RegionLock settings section");
                sawSection = true;
            }
            continue;
        }
        if (!section) continue;
        auto equal = line.find(L'=');
        if (equal == std::wstring::npos) throw std::runtime_error("Invalid settings line");
        const auto key = Trim(std::wstring_view(line).substr(0, equal));
        const auto value = Trim(std::wstring_view(line).substr(equal + 1));
        if (key == L"gamePath") {
            if (sawPath) throw std::runtime_error("Duplicate gamePath setting");
            sawPath = true;
            if (HasControl(value) || value.size() > 32767) throw std::runtime_error("Invalid game path in settings");
            settings.gamePath = value;
        } else if (key == L"allowedRegions") {
            if (sawRegions) throw std::runtime_error("Duplicate allowedRegions setting");
            sawRegions = true;
            settings.allowedRegions.clear();
            const auto text = WideToUtf8(value);
            std::set<std::string> unique;
            for (std::size_t start = 0; start < text.size();) {
                auto comma = text.find(',', start);
                if (comma == std::string::npos) comma = text.size();
                auto code = text.substr(start, comma - start);
                if (!ValidRegionCode(code) || !unique.insert(code).second || unique.size() > 256) throw std::runtime_error("Invalid allowedRegions setting");
                settings.allowedRegions.push_back(std::move(code));
                start = comma + 1;
            }
            if (!text.empty() && text.back() == ',') throw std::runtime_error("Invalid allowedRegions setting");
        }
    }
    if (!sawSection || !sawPath || !sawRegions) throw std::runtime_error("Settings file is incomplete; saved game path and region selection were not overwritten");
    return settings;
}

void SaveSettings(const Paths& paths, const Settings& settings) {
    auto gamePath = settings.gamePath.empty() ? std::wstring() : ValidateGameExecutable(settings.gamePath);
    std::string allowed;
    std::set<std::string> unique;
    for (const auto& code : settings.allowedRegions) {
        if (!ValidRegionCode(code) || !unique.insert(code).second || unique.size() > 256) throw std::runtime_error("Invalid allowed region selection");
        if (!allowed.empty()) allowed += ',';
        allowed += code;
    }
    std::wstring content = L"[RegionLock]\r\ngamePath=" + gamePath + L"\r\nallowedRegions=" + Utf8ToWide(allowed) + L"\r\n";
    std::string bytes("\xff\xfe", 2);
    bytes.append(reinterpret_cast<const char*>(content.data()), content.size() * sizeof(wchar_t));
    WriteFileAtomic(paths.settingsFile, bytes);
}

std::string DownloadCatalog(std::stop_token stop) {
    CheckStop(stop);
    const auto deadline = Clock::now() + std::chrono::seconds(45);
    const auto checkBudget = [&] {
        CheckStop(stop);
        if (Clock::now() >= deadline) throw std::runtime_error("AWS IP catalog download exceeded 45 second time budget");
    };
    HttpHandle session(WinHttpOpen(L"RegionLockNative/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value) FailWin32("Initialize HTTPS download");
    if (!WinHttpSetTimeouts(session.value, 5000, 8000, 8000, 8000)) FailWin32("Set HTTPS timeouts");
    HttpHandle connection(WinHttpConnect(session.value, L"ip-ranges.amazonaws.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) FailWin32("Connect to AWS IP catalog");
    const wchar_t* accept[] = {L"application/json", nullptr};
    HttpHandle request(WinHttpOpenRequest(connection.value, L"GET", L"/ip-ranges.json", nullptr,
        WINHTTP_NO_REFERER, accept, WINHTTP_FLAG_SECURE));
    if (!request.value) FailWin32("Create HTTPS request");
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy))) FailWin32("Disable HTTPS redirects");
    checkBudget();
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) FailWin32("Send AWS IP catalog request");
    checkBudget();
    if (!WinHttpReceiveResponse(request.value, nullptr)) FailWin32("Receive AWS IP catalog response");
    checkBudget();
    DWORD status{}, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) FailWin32("Read HTTPS response status");
    if (status != 200) throw std::runtime_error("AWS IP catalog returned HTTP " + std::to_string(status) + "; redirects are not accepted");
    DWORD contentLength{};
    length = sizeof(contentLength);
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &length, WINHTTP_NO_HEADER_INDEX) && contentLength > kMaxCatalogBytes) {
        throw std::runtime_error("AWS IP catalog exceeds 16 MiB limit");
    }
    std::string bytes;
    if (contentLength) bytes.reserve(contentLength);
    char chunk[64 * 1024];
    for (;;) {
        checkBudget();
        DWORD read{};
        if (!WinHttpReadData(request.value, chunk, sizeof(chunk), &read)) FailWin32("Read AWS IP catalog");
        checkBudget();
        if (!read) break;
        if (bytes.size() + read > kMaxCatalogBytes) throw std::runtime_error("AWS IP catalog exceeds 16 MiB limit");
        bytes.append(chunk, read);
    }
    if (bytes.empty()) throw std::runtime_error("AWS IP catalog response was empty");
    return bytes;
}

std::wstring ValidateGameExecutable(std::wstring_view input) {
    if (input.empty() || input.size() > 32767 || HasControl(input) || input.find_first_of(L"*?\"") != std::wstring_view::npos) throw std::runtime_error("Select an existing absolute game .exe path");
    std::filesystem::path path{std::wstring(input)};
    if (!path.is_absolute()) throw std::runtime_error("Game executable path must be absolute");
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (extension != L".exe") throw std::runtime_error("Game executable must have an .exe extension");
    std::error_code error;
    auto canonical = std::filesystem::canonical(path, error);
    if (error || !std::filesystem::is_regular_file(canonical, error) || error) throw std::runtime_error("Game executable does not exist or is inaccessible");
    const auto value = canonical.wstring();
    if (HasControl(value) || value.find_first_of(L"*?\"") != std::wstring::npos) throw std::runtime_error("Invalid canonical game executable path");
    return value;
}

std::wstring FindGameExecutable() {
    FileHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.valid()) {
        PROCESSENTRY32W process{sizeof(process)};
        if (Process32FirstW(snapshot.get(), &process)) {
            do {
                if (_wcsicmp(process.szExeFile, L"bf6.exe") != 0) continue;
                FileHandle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.th32ProcessID));
                if (!handle.valid()) continue;
                std::wstring value(32768, L'\0');
                DWORD count = static_cast<DWORD>(value.size());
                if (!QueryFullProcessImageNameW(handle.get(), 0, value.data(), &count)) continue;
                value.resize(count);
                try { return ValidateGameExecutable(value); } catch (const std::exception&) {}
            } while (Process32NextW(snapshot.get(), &process));
        }
    }
    // Discovery is read-only. Custom Steam libraries and EA installations can
    // always be selected explicitly in the file dialog.
    wchar_t steam[32768]{};
    DWORD bytes = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
        nullptr, steam, &bytes) == ERROR_SUCCESS) {
        try {
            return ValidateGameExecutable((std::filesystem::path(steam) / L"steamapps" / L"common" / L"Battlefield 6" / L"bf6.exe").wstring());
        } catch (const std::exception&) {}
    }
    return {};
}

std::wstring NewGeneration() {
    GUID guid{};
    CheckHr(CoCreateGuid(&guid), "Generate rule identifier");
    wchar_t text[40]{};
    if (!StringFromGUID2(guid, text, static_cast<int>(std::size(text)))) throw std::runtime_error("Format rule identifier");
    return std::wstring(text + 1, 36);
}

FirewallState QueryFirewall() {
    const std::lock_guard lock(firewallMutationMutex);
    ComApartment apartment;
    auto policy = FirewallPolicy();
    auto rules = FirewallRules(policy.get());
    const auto inventory = Inventory(rules.get(), false);
    FirewallState state;
    state.ruleCount = inventory.owned.size();
    if (const auto legacy = inventory.nameCounts.find(L"BF6_Region_Lock"); legacy != inventory.nameCounts.end()) state.legacyRuleCount = legacy->second;
    state.firewallEnabled = ActiveFirewallEnabled(policy.get());
    state.detail = ServiceProblem();
    NET_FW_MODIFY_STATE modify{};
    CheckHr(policy->get_LocalPolicyModifyState(&modify), "Read firewall policy modification state");
    if (modify != NET_FW_MODIFY_STATE_OK) state.detail += L"Local firewall policy changes are restricted. ";
    if (!state.firewallEnabled) state.detail += L"Firewall is disabled on an active network profile. ";
    if (state.detail.empty()) state.detail = L"Windows Firewall is enabled on all active network profiles.";
    return state;
}

void ApplyFirewallRules(const std::vector<RuleSpec>& specifications) {
    if (specifications.empty()) throw std::runtime_error("No firewall rules to apply; use explicit unlock instead");
    if (specifications.size() > 4096) throw std::runtime_error("Too many firewall rules");
    const std::lock_guard lock(firewallMutationMutex);
    ComApartment apartment;
    auto policy = FirewallPolicy();
    RequireWritableFirewall(policy.get(), true);
    auto rules = FirewallRules(policy.get());
    auto inventory = Inventory(rules.get(), true);
    std::vector<ComPtr<INetFwRule>> prepared;
    prepared.reserve(specifications.size());
    std::set<std::wstring, IgnoreCase> names;
    for (const auto& specification : specifications) {
        if (!names.insert(specification.name).second || inventory.nameCounts.contains(specification.name)) throw std::runtime_error("Firewall generation name collision; retry with a new generation");
        prepared.push_back(MakeRule(specification));
    }
    // Prepare every allocated name before the first policy mutation, so a
    // failed allocation cannot leave an untracked partially added generation.
    std::vector<Bstr> newNames, oldNames;
    newNames.reserve(specifications.size());
    oldNames.reserve(inventory.owned.size());
    for (const auto& spec : specifications) newNames.emplace_back(spec.name);
    for (const auto& entry : inventory.owned) oldNames.emplace_back(entry.name);
    detail::ReplaceRulesTransaction(prepared.size(), inventory.owned.size(),
        [&](std::size_t i) { CheckHr(rules->Add(prepared[i].get()), "Install new firewall generation"); },
        [&](std::size_t i) { CheckHr(rules->Remove(newNames[i].value), "Remove new firewall rule during rollback"); },
        [&](std::size_t i) { CheckHr(rules->Remove(oldNames[i].value), "Remove previous firewall generation"); },
        [&](std::size_t i) { CheckHr(rules->Add(inventory.owned[i].rule.get()), "Restore previous firewall rule during rollback"); });
}

void RemoveOwnedRules() {
    const std::lock_guard lock(firewallMutationMutex);
    ComApartment apartment;
    auto policy = FirewallPolicy();
    auto rules = FirewallRules(policy.get());
    auto inventory = Inventory(rules.get(), true);
    if (inventory.owned.empty()) return;
    // Group Policy can prevent local rules from taking effect without denying
    // deletion of those rules. Always attempt owned cleanup; COM failures are
    // handled by the same transactional rollback as a replacement operation.
    std::vector<Bstr> oldNames;
    oldNames.reserve(inventory.owned.size());
    for (const auto& entry : inventory.owned) oldNames.emplace_back(entry.name);
    detail::ReplaceRulesTransaction(0, inventory.owned.size(),
        [](std::size_t) {}, [](std::size_t) {},
        [&](std::size_t i) { CheckHr(rules->Remove(oldNames[i].value), "Remove owned region rule"); },
        [&](std::size_t i) { CheckHr(rules->Add(inventory.owned[i].rule.get()), "Restore previous firewall rule during rollback"); });
}

ProbeResult ProbeRegion(const std::string& code, std::stop_token stop) {
    ProbeResult result;
    result.code = code;
    try {
        if (!ValidRegionCode(code)) throw std::runtime_error("Invalid region code");
        if (code == "GLOBAL") throw std::runtime_error("GLOBAL AWS ranges do not have a regional GameLift probe endpoint");
        CheckStop(stop);
        Winsock winsock;
        const auto host = Utf8ToWide("gamelift-ping." + code + ".api.aws");
        auto addresses = ResolveBeacon(host, stop);
        CheckStop(stop);
        Socket socket;
        // Prefer IPv4 when both exist; IPv6-only networks/endpoints remain
        // supported. UDP connect also makes the kernel reject foreign sources.
        for (int family : {AF_INET, AF_INET6}) {
            for (auto entry = addresses.get(); entry; entry = entry->ai_next) {
                if (entry->ai_family != family) continue;
                SOCKET candidate = ::socket(entry->ai_family, SOCK_DGRAM, IPPROTO_UDP);
                if (candidate == INVALID_SOCKET) continue;
                if (::connect(candidate, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) {
                    socket.value = candidate;
                    break;
                }
                closesocket(candidate);
            }
            if (socket.value != INVALID_SOCKET) break;
        }
        if (socket.value == INVALID_SOCKET) FailWin32("Open beacon UDP endpoint", WSAGetLastError());
        u_long nonblocking = 1;
        if (ioctlsocket(socket.value, FIONBIO, &nonblocking)) FailWin32("Set UDP nonblocking mode", WSAGetLastError());
        std::array<char, 32> payload{};
        GUID token{};
        CheckHr(CoCreateGuid(&token), "Create unique probe payload");
        std::memcpy(payload.data(), &token, sizeof(token));
        std::memcpy(payload.data() + 20, "RegionLock", 10);
        std::vector<double> latencies;
        Clock::time_point previous{};
        for (int index = 0; index < result.total; ++index) {
            CheckStop(stop);
            // AWS allows at most 3 transactions per second for one source
            // IP:port. At least 350 ms between sends stays below that limit.
            if (index && !WaitUntil(previous + std::chrono::milliseconds(350), stop)) CheckStop(stop);
            std::memcpy(payload.data() + 16, &index, sizeof(index));
            const auto sent = Clock::now();
            previous = sent;
            const int written = send(socket.value, payload.data(), static_cast<int>(payload.size()), 0);
            if (written != static_cast<int>(payload.size())) continue;
            const auto deadline = sent + std::chrono::milliseconds(900);
            while (Clock::now() < deadline) {
                CheckStop(stop);
                fd_set reads;
                FD_ZERO(&reads);
                FD_SET(socket.value, &reads);
                const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
                if (remaining <= 0) break;
                timeval timeout{0, static_cast<long>(std::min<long long>(remaining, 50000))};
                const int ready = select(0, &reads, nullptr, nullptr, &timeout);
                if (ready == SOCKET_ERROR) FailWin32("Wait for UDP probe reply", WSAGetLastError());
                if (!ready) continue;
                std::array<char, 512> received{};
                const int count = recv(socket.value, received.data(), static_cast<int>(received.size()), 0);
                if (count == SOCKET_ERROR) {
                    const int error = WSAGetLastError();
                    if (error == WSAEWOULDBLOCK) continue;
                    if (error == WSAECONNRESET || error == WSAENETRESET) break;
                    FailWin32("Receive UDP probe reply", error);
                }
                if (count != static_cast<int>(payload.size()) || !std::equal(payload.begin(), payload.end(), received.begin())) continue;
                latencies.push_back(std::chrono::duration<double, std::milli>(Clock::now() - sent).count());
                ++result.received;
                break;
            }
        }
        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());
            const auto middle = latencies.size() / 2;
            result.medianMs = latencies.size() % 2 ? latencies[middle] : (latencies[middle - 1] + latencies[middle]) / 2;
        } else {
            result.error = L"No matching UDP echo replies (beacon unavailable, filtered, or timed out).";
        }
    } catch (const std::exception& error) {
        result.error = Utf8ToWide(error.what());
    }
    return result;
}
} // namespace regionlock
