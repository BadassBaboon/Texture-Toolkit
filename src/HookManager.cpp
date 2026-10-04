#include "HookManager.h"

namespace TextureToolkit
{
    HookManager &HookManager::get()
    {
        static HookManager instance;
        return instance;
    }

    HookManager::~HookManager()
    {
        shutdown();
    }

    bool HookManager::init()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
            return true;

        MH_STATUS status = MH_Initialize();
        if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED)
        {
            m_initialized = true;
            Logger::get().info("[HookManager] MinHook subsystem initialized successfully.");
            return true;
        }

        Logger::get().error("[HookManager] Failed to initialize MinHook, MH_STATUS: " + std::to_string(status));
        return false;
    }

    void HookManager::shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized)
            return;

        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        m_initialized = false;
        Logger::get().info("[HookManager] MinHook subsystem shut down successfully.");
    }
}
