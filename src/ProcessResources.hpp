// Copyright (C) 2026 AutoGavy
// SPDX-License-Identifier: AGPL-3.0-only
// See LICENSE in the project root for the full license text.

#pragma once

#include <windows.h>

namespace regionlock {
// Only changes this process. Firewall filtering runs in Windows, independently
// of the scheduling priority of the UI that installed the rules.
class ProcessResources {
public:
    ~ProcessResources() { SetBackground(false); }
    ProcessResources() = default;
    ProcessResources(const ProcessResources&) = delete;
    ProcessResources& operator=(const ProcessResources&) = delete;

    void SetBackground(bool enabled) noexcept {
        const HANDLE process = GetCurrentProcess();
        if (enabled) {
            if (!priorityCaptured_) {
                originalPriority_ = GetPriorityClass(process);
                priorityCaptured_ = originalPriority_ != 0;
            }
            if (!background_) {
                background_ = SetPriorityClass(process, PROCESS_MODE_BACKGROUND_BEGIN) != FALSE;
                backgroundError_ = background_ ? ERROR_SUCCESS : GetLastError();
            }
            // Enter background mode first: that transition can normalize the
            // CPU priority class. Apply the explicit Idle class afterwards.
            if (priorityCaptured_ && !idle_) {
                idle_ = SetPriorityClass(process, IDLE_PRIORITY_CLASS) != FALSE;
                priorityError_ = idle_ ? ERROR_SUCCESS : GetLastError();
            }
            if (!powerCaptured_) {
                originalPower_.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
                powerReadable_ = GetProcessInformation(process, ProcessPowerThrottling,
                    &originalPower_, sizeof(originalPower_)) != FALSE;
                powerError_ = powerReadable_ ? ERROR_SUCCESS : GetLastError();
                powerCaptured_ = true;
            }
            if (powerReadable_ && !eco_) {
                auto power = originalPower_;
                power.ControlMask |= PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
                power.StateMask |= PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
                eco_ = SetProcessInformation(process, ProcessPowerThrottling, &power, sizeof(power)) != FALSE;
                powerError_ = eco_ ? ERROR_SUCCESS : GetLastError();
            }
        } else {
            if (eco_ && SetProcessInformation(process, ProcessPowerThrottling,
                &originalPower_, sizeof(originalPower_))) eco_ = false;
            if (background_ && SetPriorityClass(process, PROCESS_MODE_BACKGROUND_END)) background_ = false;
            if (!background_ && idle_ && SetPriorityClass(process, originalPriority_)) idle_ = false;
            if (!background_ && !eco_ && !idle_) { powerCaptured_ = false; priorityCaptured_ = false; }
        }
    }

    bool Background() const noexcept { return background_; }
    bool Idle() const noexcept { return idle_; }
    bool Eco() const noexcept { return eco_; }
    bool PowerReadable() const noexcept { return powerReadable_; }
    DWORD BackgroundError() const noexcept { return backgroundError_; }
    DWORD PowerError() const noexcept { return powerError_; }
    DWORD PriorityError() const noexcept { return priorityError_; }

private:
    bool background_{}, idle_{}, eco_{}, powerCaptured_{}, powerReadable_{}, priorityCaptured_{};
    DWORD backgroundError_{}, powerError_{}, priorityError_{}, originalPriority_{};
    PROCESS_POWER_THROTTLING_STATE originalPower_{};
};
} // namespace regionlock
