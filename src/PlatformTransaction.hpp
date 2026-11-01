// Copyright (C) 2026 AutoGavy
// SPDX-License-Identifier: AGPL-3.0-only
// See LICENSE in the project root for the full license text.

#pragma once

#include <cstddef>
#include <exception>
#include <stdexcept>
#include <string>

namespace regionlock::detail {
inline std::string CurrentExceptionMessage() {
    try { throw; }
    catch (const std::exception& error) { return error.what(); }
    catch (...) { return "Unknown failure"; }
}

// Callbacks complete one atomic policy operation or throw before it succeeds.
// All snapshots, COM objects, and rule names must be prepared by the caller.
// This same algorithm runs against in-memory state in PlatformTests.
template<class AddNew, class RemoveNew, class RemoveOld, class RestoreOld>
void ReplaceRulesTransaction(std::size_t newCount, std::size_t oldCount,
    AddNew addNew, RemoveNew removeNew, RemoveOld removeOld, RestoreOld restoreOld) {
    std::size_t added{}, removed{};
    try {
        for (; added < newCount; ++added) addNew(added);
        for (; removed < oldCount; ++removed) removeOld(removed);
    } catch (...) {
        const auto original = CurrentExceptionMessage();
        std::string rollback;
        for (std::size_t i = 0; i < removed; ++i) {
            try { restoreOld(i); }
            catch (...) { rollback += " Restore old rule #" + std::to_string(i) + ": " + CurrentExceptionMessage(); }
        }
        // Once a complete replacement exists, never discard it if restoring
        // the old generation failed. That would leave only partial coverage.
        if (!rollback.empty() && newCount && added == newCount) {
            throw std::runtime_error(original + ". ROLLBACK INCOMPLETE:" + rollback
                + ". The complete new generation was retained; some previous rules may also remain. Unlock or reapply to recover.");
        }
        for (std::size_t i = added; i > 0; --i) {
            try { removeNew(i - 1); }
            catch (...) { rollback += " Remove new rule #" + std::to_string(i - 1) + ": " + CurrentExceptionMessage(); }
        }
        throw std::runtime_error(original + (rollback.empty()
            ? ". Previous firewall rules were preserved/restored."
            : ". ROLLBACK INCOMPLETE:" + rollback));
    }
}
} // namespace regionlock::detail
