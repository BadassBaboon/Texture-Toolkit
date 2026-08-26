/*
 * Texture Toolkit Standalone v1.1.0 by BadassBaboon
 * Native Proxy Wrapper & ASI Plugin for Direct3D 9 & Direct3D 11
 */

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include "Config.h"
#include "Logger.h"
#include "HookManager.h"
#include "TextureManager.h"
#include "D3D9Hook.h"
#include "D3D11Hook.h"
#include "DInput8Hook.h"

#include "Version.h"

#include <thread>
#include <atomic>

// Handle to our own module (.asi). Set in DllMain before initialize_standalone runs.
HMODULE g_our_module = nullptr;

namespace TextureToolkit
{
    // Reports once, a little after startup, whether we actually ended up on the game's render
    // path. A log that stops after "hooks installed" is ambiguous: it could mean the game crashed,
    // that it renders through an API we did not hook, or simply that the player quit. This turns
    // that guesswork into one line. Runs on its own thread so it cannot delay or affect the game;
    // a thread created during DllMain does not start until the loader lock is released, and the
    // OS terminates it before static teardown, so it can never outlive the Logger.
    static void start_startup_watchdog()
    {
        std::thread([]()
        {
            constexpr int kReportAfterSeconds = 20;
            for (int i = 0; i < kReportAfterSeconds; ++i)
                Sleep(1000);

            const uint64_t frames9 = D3D9Hook::s_present_count.load(std::memory_order_relaxed);
            const uint64_t frames11 = D3D11Hook::s_present_count.load(std::memory_order_relaxed);
            const bool have_d3d9 = D3D9Hook::get().get_device() != nullptr;
            const bool have_d3d11 = D3D11Hook::get().get_device() != nullptr;
            const bool overlay = D3D9Hook::get().overlay_ready() || D3D11Hook::get().overlay_ready();

            Logger::get().info("[Watchdog] " + std::to_string(kReportAfterSeconds) + "s status: d3d9_device=" +
                               std::to_string(have_d3d9 ? 1 : 0) + " d3d11_device=" + std::to_string(have_d3d11 ? 1 : 0) +
                               " frames_d3d9=" + std::to_string(frames9) + " frames_d3d11=" + std::to_string(frames11) +
                               " overlay=" + std::to_string(overlay ? 1 : 0));

            if (frames9 == 0 && frames11 == 0)
            {
                if (!have_d3d9 && !have_d3d11)
                    Logger::get().warn("[Watchdog] No Direct3D 9 or 11 device was created. Either the game had not "
                                       "finished starting, or it renders with an API Texture Toolkit does not hook "
                                       "(DirectX 8/10/12 or Vulkan).");
                else
                    Logger::get().warn("[Watchdog] A device exists but no frame has been presented through our hook. "
                                       "The game is rendering somewhere we are not on the path (another overlay may "
                                       "own Present), or it stopped before its first frame.");
            }
            else if (!overlay)
            {
                Logger::get().warn("[Watchdog] Frames are being presented but the overlay never initialised; the "
                                   "panel will not appear.");
            }
        }).detach();
    }

    void initialize_standalone()
    {
        wchar_t exe_path[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe_path, ARRAYSIZE(exe_path));
        std::filesystem::path game_dir = std::filesystem::path(exe_path).parent_path();

        // Keep the .ini and .log next to the .asi (usually the plugins/ or scripts/ folder),
        // so they are easy to find and the dump/inject folders can be re-pointed in the .ini.
        wchar_t module_path[MAX_PATH] = L"";
        GetModuleFileNameW(g_our_module, module_path, ARRAYSIZE(module_path));
        std::filesystem::path asi_dir = std::filesystem::path(module_path).parent_path();
        if (asi_dir.empty())
            asi_dir = game_dir;

        Logger::get().init(asi_dir);

        // Identify the exact binary and host up front. A user's log should answer "which build,
        // loaded how, into what" without having to ask them: that was the missing information in
        // more than one bug report.
        Logger::get().info(std::string("[Main] Texture Toolkit v") + TT_VERSION_STRING +
                           " (" + (sizeof(void *) == 8 ? "x64" : "x86") +
                           ", built " __DATE__ " " __TIME__ ") initializing...");
        Logger::get().info("[Main] Host process: " + std::filesystem::path(exe_path).string());
        Logger::get().info("[Main] Loaded from:   " + std::filesystem::path(module_path).string());

        ConfigManager::get().init(asi_dir);
        Logger::get().set_min_level(ConfigManager::get().get_config().verbose ? LogLevel::Debug : LogLevel::Info);

        TextureManager::get().init();
        HookManager::get().init();

        DInput8Hook::get().init();
        D3D9Hook::get().init();
        D3D11Hook::get().init();

        Logger::get().info("[Main] Initialization complete. Press " + hotkey_name(ConfigManager::get().get_config().hotkey) + " to open the Texture Toolkit panel.");

        start_startup_watchdog();
    }

    void shutdown_standalone()
    {
        Logger::get().info("[Main] Texture Toolkit Standalone shutting down...");
        TextureManager::get().shutdown();
        DInput8Hook::get().shutdown();
        D3D9Hook::get().shutdown();
        D3D11Hook::get().shutdown();
        HookManager::get().shutdown();
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        g_our_module = hModule;
        DisableThreadLibraryCalls(hModule);
        // We MUST initialize synchronously so that our hooks are installed
        // BEFORE the game continues its execution and calls D3D initialization functions!
        TextureToolkit::initialize_standalone();
        break;

    case DLL_PROCESS_DETACH:
        // lpvReserved != NULL means the PROCESS is terminating. Per MSDN, we must NOT do
        // cleanup here: the OS is already tearing the process down and every other thread
        // has been terminated, so joining our worker thread or calling COM Release()/D3D
        // teardown under the loader lock deadlocks (observed as a black-screen hang when
        // quitting Bully). The OS reclaims all of it anyway. Only clean up on an explicit
        // FreeLibrary unload (lpvReserved == NULL), which is the safe, orderly case.
        if (lpvReserved == nullptr)
            TextureToolkit::shutdown_standalone();
        break;
    }

    return TRUE;
}
