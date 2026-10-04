#include "DInput8Hook.h"
#include "HookManager.h"
#include "IATHook.h"
#include "TextureToolkitUI.h"
#include "Config.h"
#include "Logger.h"
#include "ScopedFlag.h"
#include <atomic>
#include <intrin.h>
#include "imgui.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

extern HMODULE g_our_module;

namespace TextureToolkit
{
    // Gates the hooked GetAsyncKeyState/GetKeyState so our own polling sees real key state while
    // the game's does not. Per thread, so raising it on the render thread (or the thread pumping
    // the window's messages) never opens the mask for a game thread polling keys meanwhile.
    thread_local bool g_inside_imgui_render = false;
    DInput8Hook &DInput8Hook::get()
    {
        static DInput8Hook instance;
        return instance;
    }

    DInput8Hook::~DInput8Hook()
    {
        shutdown();
    }

    bool DInput8Hook::init()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
            return true;

        // 1. Hook DInput8
        HMODULE dinput8_module = GetModuleHandleA("dinput8.dll");
        if (dinput8_module == nullptr)
        {
            dinput8_module = LoadLibraryA("dinput8.dll");
        }

        if (dinput8_module != nullptr)
        {
            void *pDirectInput8Create = reinterpret_cast<void *>(GetProcAddress(dinput8_module, "DirectInput8Create"));
            if (pDirectInput8Create != nullptr)
            {
                HookManager::get().create_hook(pDirectInput8Create, &Hooked_DirectInput8Create, reinterpret_cast<void **>(&m_orig_dinput8_create));
                IATHook::hook_all_modules("dinput8.dll", "DirectInput8Create", &Hooked_DirectInput8Create, reinterpret_cast<void **>(&m_orig_dinput8_create));
                Logger::get().info("[DInput8Hook] DirectInput8Create API & IAT hooks installed successfully.");
            }
        }

        // 2. Hook User32 APIs
        HMODULE user32_module = GetModuleHandleA("user32.dll");
        if (user32_module != nullptr)
        {
            void *pSetCursorPos = reinterpret_cast<void *>(GetProcAddress(user32_module, "SetCursorPos"));
            if (pSetCursorPos != nullptr)
            {
                HookManager::get().create_hook(pSetCursorPos, &Hooked_SetCursorPos, reinterpret_cast<void **>(&m_orig_set_cursor_pos));
            }

            void *pClipCursor = reinterpret_cast<void *>(GetProcAddress(user32_module, "ClipCursor"));
            if (pClipCursor != nullptr)
            {
                HookManager::get().create_hook(pClipCursor, &Hooked_ClipCursor, reinterpret_cast<void **>(&m_orig_clip_cursor));
            }

            void *pPeekMessageA = reinterpret_cast<void *>(GetProcAddress(user32_module, "PeekMessageA"));
            if (pPeekMessageA != nullptr)
            {
                HookManager::get().create_hook(pPeekMessageA, &Hooked_PeekMessageA, reinterpret_cast<void **>(&m_orig_peek_message_a));
            }

            void *pPeekMessageW = reinterpret_cast<void *>(GetProcAddress(user32_module, "PeekMessageW"));
            if (pPeekMessageW != nullptr)
            {
                HookManager::get().create_hook(pPeekMessageW, &Hooked_PeekMessageW, reinterpret_cast<void **>(&m_orig_peek_message_w));
            }

            void *pGetMessageA = reinterpret_cast<void *>(GetProcAddress(user32_module, "GetMessageA"));
            if (pGetMessageA != nullptr)
            {
                HookManager::get().create_hook(pGetMessageA, &Hooked_GetMessageA, reinterpret_cast<void **>(&m_orig_get_message_a));
            }

            void *pGetMessageW = reinterpret_cast<void *>(GetProcAddress(user32_module, "GetMessageW"));
            if (pGetMessageW != nullptr)
            {
                HookManager::get().create_hook(pGetMessageW, &Hooked_GetMessageW, reinterpret_cast<void **>(&m_orig_get_message_w));
            }

            void *pGetAsyncKeyState = reinterpret_cast<void *>(GetProcAddress(user32_module, "GetAsyncKeyState"));
            if (pGetAsyncKeyState != nullptr)
            {
                HookManager::get().create_hook(pGetAsyncKeyState, &Hooked_GetAsyncKeyState, reinterpret_cast<void **>(&m_orig_get_async_key_state));
            }

            void *pGetKeyState = reinterpret_cast<void *>(GetProcAddress(user32_module, "GetKeyState"));
            if (pGetKeyState != nullptr)
            {
                HookManager::get().create_hook(pGetKeyState, &Hooked_GetKeyState, reinterpret_cast<void **>(&m_orig_get_key_state));
            }

            void *pGetKeyboardState = reinterpret_cast<void *>(GetProcAddress(user32_module, "GetKeyboardState"));
            if (pGetKeyboardState != nullptr)
            {
                HookManager::get().create_hook(pGetKeyboardState, &Hooked_GetKeyboardState, reinterpret_cast<void **>(&m_orig_get_keyboard_state));
            }

            Logger::get().info("[DInput8Hook] User32 cursor, input and message hooks installed successfully.");
        }

        m_initialized = true;
        return true;
    }

    void DInput8Hook::shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_initialized = false;
    }

    void DInput8Hook::hook_dinput8_interface(IDirectInput8 *dinput)
    {
        if (dinput == nullptr) return;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_hooked_dinput8_interfaces.count(dinput)) return;

        void **vtable = *reinterpret_cast<void ***>(dinput);
        void *create_device_addr = vtable[3]; // IDirectInput8::CreateDevice is index 3

        if (m_orig_create_device == nullptr)
        {
            HookManager::get().create_hook(create_device_addr, &Hooked_CreateDevice, reinterpret_cast<void **>(&m_orig_create_device));
            Logger::get().info("[DInput8Hook] Intercepted IDirectInput8::CreateDevice.");
        }

        m_hooked_dinput8_interfaces.insert(dinput);
    }

    void DInput8Hook::hook_device_interface(IDirectInputDevice8 *device)
    {
        if (device == nullptr) return;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_hooked_devices.count(device)) return;

        void **vtable = *reinterpret_cast<void ***>(device);
        void *get_state_addr = vtable[9]; // IDirectInputDevice8::GetDeviceState is index 9
        void *get_data_addr = vtable[10]; // IDirectInputDevice8::GetDeviceData is index 10

        if (m_orig_get_device_state == nullptr)
        {
            HookManager::get().create_hook(get_state_addr, &Hooked_GetDeviceState, reinterpret_cast<void **>(&m_orig_get_device_state));
            HookManager::get().create_hook(get_data_addr, &Hooked_GetDeviceData, reinterpret_cast<void **>(&m_orig_get_device_data));
            Logger::get().info("[DInput8Hook] REAL GAME DINPUT8 DEVICE INTERCEPTED! GetDeviceState/GetDeviceData hooked.");
        }

        m_hooked_devices.insert(device);
    }

    HRESULT WINAPI DInput8Hook::Hooked_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID *ppvOut, LPUNKNOWN punkOuter)
    {
        HRESULT hr = E_FAIL;
        if (get().m_orig_dinput8_create)
        {
            hr = get().m_orig_dinput8_create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
        }

        if (SUCCEEDED(hr) && ppvOut != nullptr && *ppvOut != nullptr)
        {
            get().hook_dinput8_interface(static_cast<IDirectInput8 *>(*ppvOut));
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE DInput8Hook::Hooked_CreateDevice(IDirectInput8 *pThis, REFGUID rguid, LPDIRECTINPUTDEVICE8 *lplpDirectInputDevice, LPUNKNOWN pUnkOuter)
    {
        HRESULT hr = E_FAIL;
        if (get().m_orig_create_device)
        {
            hr = get().m_orig_create_device(pThis, rguid, lplpDirectInputDevice, pUnkOuter);
        }

        if (SUCCEEDED(hr) && lplpDirectInputDevice != nullptr && *lplpDirectInputDevice != nullptr)
        {
            get().hook_device_interface(*lplpDirectInputDevice);
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE DInput8Hook::Hooked_GetDeviceState(IDirectInputDevice8 *pThis, DWORD cbData, LPVOID lpvData)
    {
        HRESULT hr = get().m_orig_get_device_state(pThis, cbData, lpvData);

        if (SUCCEEDED(hr) && TextureToolkitUI::is_visible())
        {
            // Block input to the game by clearing the buffer!
            if (lpvData != nullptr && cbData > 0)
            {
                memset(lpvData, 0, cbData);
            }
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE DInput8Hook::Hooked_GetDeviceData(IDirectInputDevice8 *pThis, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags)
    {
        HRESULT hr = get().m_orig_get_device_data(pThis, cbObjectData, rgdod, pdwInOut, dwFlags);

        if (SUCCEEDED(hr) && TextureToolkitUI::is_visible())
        {
            // Block input by simulating zero events read
            if (pdwInOut != nullptr)
            {
                *pdwInOut = 0;
            }
        }

        return hr;
    }



    BOOL WINAPI DInput8Hook::Hooked_SetCursorPos(int X, int Y)
    {
        if (TextureToolkitUI::is_visible())
        {
            return TRUE; // Ignore and report success (blocks game cursor centering)
        }
        return get().m_orig_set_cursor_pos(X, Y);
    }

    BOOL WINAPI DInput8Hook::Hooked_ClipCursor(const RECT *lpRect)
    {
        if (TextureToolkitUI::is_visible())
        {
            return TRUE; // Block game cursor clipping
        }
        return get().m_orig_clip_cursor(lpRect);
    }

    BOOL WINAPI DInput8Hook::Hooked_PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
    {
        BOOL ret = get().m_orig_peek_message_a(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
        if (ret && TextureToolkitUI::is_visible() && lpMsg != nullptr)
        {
            if (handle_input_message(lpMsg, (wRemoveMsg & PM_REMOVE) != 0, true))
            {
                lpMsg->message = WM_NULL;
            }
        }
        return ret;
    }

    BOOL WINAPI DInput8Hook::Hooked_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
    {
        BOOL ret = get().m_orig_peek_message_w(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
        if (ret && TextureToolkitUI::is_visible() && lpMsg != nullptr)
        {
            if (handle_input_message(lpMsg, (wRemoveMsg & PM_REMOVE) != 0, false))
            {
                lpMsg->message = WM_NULL;
            }
        }
        return ret;
    }

    BOOL WINAPI DInput8Hook::Hooked_GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
    {
        BOOL ret = get().m_orig_get_message_a(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
        if (ret > 0 && TextureToolkitUI::is_visible() && lpMsg != nullptr)
        {
            if (handle_input_message(lpMsg, true, true))
            {
                lpMsg->message = WM_NULL;
            }
        }
        return ret;
    }

    BOOL WINAPI DInput8Hook::Hooked_GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
    {
        BOOL ret = get().m_orig_get_message_w(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
        if (ret > 0 && TextureToolkitUI::is_visible() && lpMsg != nullptr)
        {
            if (handle_input_message(lpMsg, true, false))
            {
                lpMsg->message = WM_NULL;
            }
        }
        return ret;
    }

    SHORT WINAPI DInput8Hook::Hooked_GetAsyncKeyState(int vKey)
    {
        if (TextureToolkitUI::is_visible() && !g_inside_imgui_render)
        {
            if (vKey != static_cast<int>(ConfigManager::get().get_config().hotkey))
            {
                return 0;
            }
        }
        return get().m_orig_get_async_key_state(vKey);
    }

    SHORT WINAPI DInput8Hook::Hooked_GetKeyState(int vKey)
    {
        if (TextureToolkitUI::is_visible() && !g_inside_imgui_render)
        {
            if (vKey != static_cast<int>(ConfigManager::get().get_config().hotkey))
            {
                return 0;
            }
        }
        return get().m_orig_get_key_state(vKey);
    }

    BOOL WINAPI DInput8Hook::Hooked_GetKeyboardState(PBYTE lpKeyState)
    {
        BOOL ret = get().m_orig_get_keyboard_state(lpKeyState);
        if (ret && TextureToolkitUI::is_visible() && !g_inside_imgui_render)
        {
            int toggle_key = static_cast<int>(ConfigManager::get().get_config().hotkey);
            BYTE toggle_state = lpKeyState[toggle_key];
            memset(lpKeyState, 0, 256);
            lpKeyState[toggle_key] = toggle_state;
        }
        return ret;
    }

    // Takes a message the game just fetched from its queue while the panel is open, hands it to
    // ImGui, and says whether to blank it so the game never acts on it.
    //
    // Keyboard input is blocked here, in the queue, rather than at dispatch, so games that read
    // keys straight out of their pump without dispatching are covered too. The catch is that a key
    // press only becomes a character when the game's pump passes it to TranslateMessage, and a
    // blanked message never gets there: the panel's search box received no text. So a key press is
    // translated here first, which posts its WM_CHAR to the queue, where it arrives on the next pump
    // and is handed to ImGui and blanked in turn. Blanking WM_SYSCHAR as well is what keeps Windows
    // from beeping at an Alt+key typed into the panel.
    //
    // `removed` is false for a PeekMessage that leaves the message queued: it will be fetched again,
    // so it is blanked now but fed and translated only once, when it is actually taken. `ansi` says
    // which encoding a character arrived in (PeekMessageA/GetMessageA deliver the code page's,
    // whatever the window's own is), since the ImGui backend assumes the window's.
    bool DInput8Hook::handle_input_message(LPMSG lpMsg, bool removed, bool ansi)
    {
        if (lpMsg == nullptr) return false;

        if (lpMsg->message == WM_INPUT)
            return true;

        const UINT msg = lpMsg->message;
        if (!((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)))
            return false;

        if (!removed)
            return true;

        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
            TranslateMessage(lpMsg);

        if (msg == WM_CHAR)
        {
            ImGuiIO &io = ImGui::GetIO();
            if (!ansi)
            {
                io.AddInputCharacterUTF16(static_cast<ImWchar16>(lpMsg->wParam));
            }
            else
            {
                // A double-byte code page sends a character as a lead byte and a trail byte, in
                // two messages.
                static thread_local char s_lead = 0;
                const char byte = static_cast<char>(lpMsg->wParam & 0xFF);
                char bytes[2] = { byte, 0 };
                int len = 1;
                if (s_lead != 0)
                {
                    bytes[0] = s_lead;
                    bytes[1] = byte;
                    len = 2;
                    s_lead = 0;
                }
                else if (IsDBCSLeadByte(static_cast<BYTE>(byte)))
                {
                    s_lead = byte;
                    return true;
                }
                wchar_t wide[2] = {};
                const int n = MultiByteToWideChar(CP_ACP, 0, bytes, len, wide, 2);
                for (int i = 0; i < n; ++i)
                    io.AddInputCharacterUTF16(static_cast<ImWchar16>(wide[i]));
            }
            return true;
        }

        if (msg == WM_SYSCHAR || msg == WM_DEADCHAR || msg == WM_SYSDEADCHAR)
            return true; // not text for the panel; blanked so DefWindowProc never beeps at it

        ScopedFlag reading_real_input(g_inside_imgui_render);
        ImGui_ImplWin32_WndProcHandler(lpMsg->hwnd, msg, lpMsg->wParam, lpMsg->lParam);
        return true; // Block input message from reaching the game
    }
}
