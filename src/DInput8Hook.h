#pragma once

#include <windows.h>
#include <dinput.h>
#include <mutex>
#include <unordered_set>

namespace TextureToolkit
{
    class DInput8Hook
    {
    public:
        static DInput8Hook &get();
        ~DInput8Hook();

        bool init();
        void shutdown();

        void hook_dinput8_interface(IDirectInput8 *dinput);
        void hook_device_interface(IDirectInputDevice8 *device);

    private:
        DInput8Hook() = default;

        std::mutex m_mutex;
        bool m_initialized = false;

        // Original function pointers (DInput8)
        typedef HRESULT(WINAPI *DirectInput8Create_t)(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID *ppvOut, LPUNKNOWN punkOuter);
        DirectInput8Create_t m_orig_dinput8_create = nullptr;

        typedef HRESULT(STDMETHODCALLTYPE *CreateDevice_t)(IDirectInput8 *pThis, REFGUID rguid, LPDIRECTINPUTDEVICE8 *lplpDirectInputDevice, LPUNKNOWN pUnkOuter);
        CreateDevice_t m_orig_create_device = nullptr;

        typedef HRESULT(STDMETHODCALLTYPE *GetDeviceState_t)(IDirectInputDevice8 *pThis, DWORD cbData, LPVOID lpvData);
        GetDeviceState_t m_orig_get_device_state = nullptr;

        typedef HRESULT(STDMETHODCALLTYPE *GetDeviceData_t)(IDirectInputDevice8 *pThis, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags);
        GetDeviceData_t m_orig_get_device_data = nullptr;



        typedef BOOL(WINAPI *SetCursorPos_t)(int X, int Y);
        SetCursorPos_t m_orig_set_cursor_pos = nullptr;

        typedef BOOL(WINAPI *ClipCursor_t)(const RECT *lpRect);
        ClipCursor_t m_orig_clip_cursor = nullptr;

        // User32 Message Hooks
        typedef BOOL(WINAPI *PeekMessageA_t)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
        PeekMessageA_t m_orig_peek_message_a = nullptr;

        typedef BOOL(WINAPI *PeekMessageW_t)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
        PeekMessageW_t m_orig_peek_message_w = nullptr;

        typedef BOOL(WINAPI *GetMessageA_t)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
        GetMessageA_t m_orig_get_message_a = nullptr;

        typedef BOOL(WINAPI *GetMessageW_t)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
        GetMessageW_t m_orig_get_message_w = nullptr;

        // User32 Input Hooks
        typedef SHORT(WINAPI *GetAsyncKeyState_t)(int vKey);
        GetAsyncKeyState_t m_orig_get_async_key_state = nullptr;

        typedef SHORT(WINAPI *GetKeyState_t)(int vKey);
        GetKeyState_t m_orig_get_key_state = nullptr;

        typedef BOOL(WINAPI *GetKeyboardState_t)(PBYTE lpKeyState);
        GetKeyboardState_t m_orig_get_keyboard_state = nullptr;

        // Hooked functions
        static HRESULT WINAPI Hooked_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID *ppvOut, LPUNKNOWN punkOuter);
        static HRESULT STDMETHODCALLTYPE Hooked_CreateDevice(IDirectInput8 *pThis, REFGUID rguid, LPDIRECTINPUTDEVICE8 *lplpDirectInputDevice, LPUNKNOWN pUnkOuter);
        static HRESULT STDMETHODCALLTYPE Hooked_GetDeviceState(IDirectInputDevice8 *pThis, DWORD cbData, LPVOID lpvData);
        static HRESULT STDMETHODCALLTYPE Hooked_GetDeviceData(IDirectInputDevice8 *pThis, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags);

        static BOOL WINAPI Hooked_SetCursorPos(int X, int Y);
        static BOOL WINAPI Hooked_ClipCursor(const RECT *lpRect);

        static SHORT WINAPI Hooked_GetAsyncKeyState(int vKey);
        static SHORT WINAPI Hooked_GetKeyState(int vKey);
        static BOOL WINAPI Hooked_GetKeyboardState(PBYTE lpKeyState);

        static BOOL WINAPI Hooked_PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
        static BOOL WINAPI Hooked_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
        static BOOL WINAPI Hooked_GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
        static BOOL WINAPI Hooked_GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);

        static bool handle_input_message(LPMSG lpMsg, bool removed);

        std::unordered_set<IDirectInput8 *> m_hooked_dinput8_interfaces;
        std::unordered_set<IDirectInputDevice8 *> m_hooked_devices;
    };
}
