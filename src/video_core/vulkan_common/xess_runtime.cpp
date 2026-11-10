// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/xess_runtime.h"

#ifdef _WIN32
#include <array>
#include <string>

#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#endif

namespace Vulkan::XessRuntime {

bool Wanted() {
    const char* const value = std::getenv("EDEN_NX2_XESS");
    if (value != nullptr && *value != '\0') {
        return std::strcmp(value, "0") != 0;
    }
    return Settings::values.intel_xess.GetValue();
}

std::string_view ResultName(xess_result_t result) {
    switch (result) {
    case XESS_RESULT_WARNING_NONEXISTING_FOLDER:
        return "warning: non-existing folder";
    case XESS_RESULT_WARNING_OLD_DRIVER:
        return "warning: old driver";
    case XESS_RESULT_SUCCESS:
        return "success";
    case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE:
        return "unsupported device";
    case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER:
        return "unsupported driver";
    case XESS_RESULT_ERROR_UNINITIALIZED:
        return "uninitialized";
    case XESS_RESULT_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY:
        return "device out of memory";
    case XESS_RESULT_ERROR_DEVICE:
        return "device error";
    case XESS_RESULT_ERROR_NOT_IMPLEMENTED:
        return "not implemented";
    case XESS_RESULT_ERROR_INVALID_CONTEXT:
        return "invalid context";
    case XESS_RESULT_ERROR_OPERATION_IN_PROGRESS:
        return "operation in progress";
    case XESS_RESULT_ERROR_UNSUPPORTED:
        return "unsupported";
    case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY:
        return "can't load library";
    case XESS_RESULT_ERROR_WRONG_CALL_ORDER:
        return "wrong call order";
    default:
        return "unknown error";
    }
}

#ifdef _WIN32
namespace {

bool IsSignedByIntel(const std::filesystem::path& path) {
    const std::wstring file_path = std::filesystem::absolute(path).wstring();
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = file_path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool valid = WinVerifyTrust(nullptr, &action, &data) == ERROR_SUCCESS;
    if (valid) {
        CRYPT_PROVIDER_DATA* const provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* const signer =
            provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* const cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        wchar_t name[128]{};
        valid = cert &&
                CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name,
                                   static_cast<DWORD>(std::size(name))) > 1 &&
                std::wstring_view{name} == L"Intel Corporation";
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return valid;
}

template <typename T>
bool Resolve(HMODULE module, const char* name, T& function) {
    function = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    if (function == nullptr) {
        LOG_WARNING(Render_Vulkan, "Intel XeSS: libxess.dll lacks {}", name);
        return false;
    }
    return true;
}

std::optional<Api> Load() {
    const std::array<std::filesystem::path, 2> candidates{
        Common::FS::GetExeDirectory() / DLL_NAME,
        Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "xess" / DLL_NAME,
    };
    for (const std::filesystem::path& path : candidates) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            continue;
        }
        const std::string shown = Common::FS::PathToUTF8String(path);
        if (!IsSignedByIntel(path)) {
            LOG_WARNING(Render_Vulkan, "Intel XeSS: {} isn't signed by Intel; not loading it",
                        shown);
            continue;
        }
        const HMODULE module =
            LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module == nullptr) {
            LOG_WARNING(Render_Vulkan, "Intel XeSS: {} doesn't load (error {})", shown,
                        GetLastError());
            continue;
        }
        Api api{};
        const bool complete =
            Resolve(module, "xessGetVersion", api.get_version) &&
            Resolve(module, "xessGetIntelXeFXVersion", api.get_xefx_version) &&
            Resolve(module, "xessGetOptimalInputResolution", api.get_optimal_input_resolution) &&
            Resolve(module, "xessDestroyContext", api.destroy_context) &&
            Resolve(module, "xessSetLoggingCallback", api.set_logging_callback) &&
            Resolve(module, "xessIsOptimalDriver", api.is_optimal_driver) &&
            Resolve(module, "xessSetJitterScale", api.set_jitter_scale) &&
            Resolve(module, "xessSetVelocityScale", api.set_velocity_scale) &&
            Resolve(module, "xessVKGetRequiredInstanceExtensions",
                    api.vk_get_required_instance_extensions) &&
            Resolve(module, "xessVKGetRequiredDeviceExtensions",
                    api.vk_get_required_device_extensions) &&
            Resolve(module, "xessVKGetRequiredDeviceFeatures",
                    api.vk_get_required_device_features) &&
            Resolve(module, "xessVKCreateContext", api.vk_create_context) &&
            Resolve(module, "xessVKInit", api.vk_init) &&
            Resolve(module, "xessVKExecute", api.vk_execute);
        if (!complete) {
            FreeLibrary(module);
            continue;
        }
        xess_version_t version{};
        api.get_version(&version);
        LOG_INFO(Render_Vulkan, "Intel XeSS: SDK {}.{}.{} runtime loaded from {}", version.major,
                 version.minor, version.patch, shown);
        return api; // kept loaded for the process: the Vulkan device was made for it
    }
    LOG_WARNING(Render_Vulkan,
                "Intel XeSS: no usable libxess.dll next to lichii.exe or in the xess folder of "
                "Lichii's data folder; using FSR");
    return std::nullopt;
}

} // Anonymous namespace

const Api* Get() {
    static const std::optional<Api> api = Load();
    return api ? &*api : nullptr;
}

#else

const Api* Get() {
    static std::once_flag logged;
    std::call_once(logged, [] {
        LOG_INFO(Render_Vulkan, "Intel XeSS: Intel's runtime is Windows-only; using FSR");
    });
    return nullptr;
}

#endif

} // namespace Vulkan::XessRuntime
