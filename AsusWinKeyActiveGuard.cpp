#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h> // 新增：用于深度获取 Usage Page 特征
#include <shellapi.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <shobjidl.h> // 新增：用于修改通知中心的应用名称
#include "resource.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// ==============================================================================
// 内部消息与宏定义
// ==============================================================================
#define WM_TRAYICON         (WM_USER + 1)
#define WM_USER_NOTIFY      (WM_USER + 2)
#define WM_USER_LOG         (WM_USER + 3)
#define WM_USER_ENUM_DONE   (WM_USER + 4) // 枚举完成回调

#define ID_TRAY_STATE       3001
#define ID_TRAY_TOGGLE_LOCK 3002
#define ID_TRAY_DEFENSE     3003
#define ID_TRAY_RESTORE     3004
#define ID_TRAY_SETTINGS    3005
#define ID_TRAY_EXIT        3006

#define ID_BTN_ENUM         2001
#define ID_BTN_APPLY        2002
#define ID_CHK_DEFENSE      2003
#define ID_CHK_NOTIFY       2004
#define ID_EDIT_TIME        2005
#define ID_EDIT_COUNT       2006
#define ID_LINK_GITHUB      2007

// ? 替换这 4 行，将 ID 改到 4000 系列以防碰撞
#define ID_CHK_ALERT    4008
#define ID_LBL_DEFENSE  4001
#define ID_LBL_NOTIFY   4002
#define ID_LBL_ALERT    4003

#define ID_CHK_STARTUP  4010 // 找个地方加上宏定义

enum NotifyType { NOTIFY_SUCCESS = 1, NOTIFY_WARNING = 2, NOTIFY_ERROR = 3 };

// 列表互动状态机
enum TestState {
    STATE_UNTESTED = 0,
    STATE_TESTING = 1, // 显示红色的[有效]和灰色的[无效]
    STATE_INVALID = 2,
    STATE_VALID = 3
};

struct ScannedDevice {
    std::wstring path;
    TestState state;
};

HINSTANCE hInst;
HWND g_hMainWnd = NULL;
HWND g_hSettingsWnd = NULL;
bool g_bKilledOldInstance = false; // 记录是否杀死了旧实例
NOTIFYICONDATAW g_nid = { 0 };
HWND g_hMenuToolTip = NULL;
TOOLINFO g_tiMenu = { sizeof(TOOLINFO) };
HWND hListDevices, hLblCustom, hEditCustom, hBtnEnum, hBtnCustomApply;
// 新增：关于窗口内部子控件句柄，用于动态 DPI 布局重排
HWND hSetLblTitle = NULL, hSetLblAuthor = NULL, hSetLblGit = NULL, hSetImgLogo = NULL;
HWND hChkDefense, hChkNotify;
HWND hLblTime, hEditTime, hLblCount, hEditCount, hLblTail;
HWND hLogEdit;

HWND hChkStartup = NULL;

HFONT g_hFontMain = NULL;
HFONT g_hFontSettings = NULL;
bool g_bIsInitializing = false; // 用于拦截 UI 创建时的幽灵 EN_CHANGE 消息

// 记录窗口是否是第一次被显示，用于白嫖系统的原生 DWM 弹出动画
bool g_bIsFirstShowMain = true;
bool g_bIsFirstShowSettings = true;

std::wstring g_CurrentDevicePath = L"";
std::atomic<bool> g_Running(true);
std::atomic<bool> g_ActiveDefense(true);
std::atomic<bool> g_EnableNotify(true);
std::atomic<bool> g_IsLocked(false);
std::atomic<int> g_AlertTimeMs(1000);
std::atomic<int> g_AlertCount(5);
// 在原有的全局变量区 (std::atomic<bool> g_ActiveDefense...) 下面加上这行：
std::atomic<bool> g_EnableAlert(true);
HWND hChkAlert = NULL; // 全局句柄

HANDLE g_hDevice = INVALID_HANDLE_VALUE;
std::thread g_MonitorThread;
std::mutex g_LogMutex;

std::vector<ScannedDevice> g_ScannedDevices;

// 主题引擎
bool g_IsDarkMode = false;
HBRUSH g_hBgBrush = NULL;
HBRUSH g_hEditBgBrush = NULL;
COLORREF g_TextColor = RGB(0, 0, 0);
COLORREF g_BgColor = RGB(255, 255, 255);

std::wstring GetStringRes(UINT id) {
    wchar_t buffer[512] = { 0 };
    LoadStringW(GetModuleHandle(NULL), id, buffer, 512);
    return std::wstring(buffer);
}

// 设置或取消开机自启（同时写入系统 Run 键 和 程序的专属注册表领地）
void SetAutoStart(bool enable) {
    HKEY hRunKey, hAppKey;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    // 1. 操作系统级别的 Run 键
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hRunKey) == ERROR_SUCCESS) {
        if (enable) {
            std::wstring pathStr = L"\""; pathStr += exePath; pathStr += L"\""; // -minimized
            RegSetValueExW(hRunKey, L"WinKeyDefender", 0, REG_SZ, (BYTE*)pathStr.c_str(), (pathStr.length() + 1) * sizeof(wchar_t));
        } else {
            RegDeleteValueW(hRunKey, L"WinKeyDefender");
        }
        RegCloseKey(hRunKey);
    }

    // 2. 操作程序专属配置区（用于记录当时的物理路径）
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\WinKeyDefender", 0, NULL, 0, KEY_WRITE, NULL, &hAppKey, NULL) == ERROR_SUCCESS) {
        if (enable) {
            RegSetValueExW(hAppKey, L"AutoStartPath", 0, REG_SZ, (BYTE*)exePath, (wcslen(exePath) + 1) * sizeof(wchar_t));
        } else {
            RegDeleteValueW(hAppKey, L"AutoStartPath");
        }
        RegCloseKey(hAppKey);
    }
}

// 启动时核对状态：同步拦截、处理文件移动
bool SyncAndCheckAutoStart(HWND hWnd) {
    HKEY hRunKey, hAppKey;
    bool inRunKey = false, inAppKey = false;
    std::wstring appSavedPath = L"";

    // 检查系统 Run 键是否还有我们的自启项
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hRunKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hRunKey, L"WinKeyDefender", NULL, NULL, NULL, NULL) == ERROR_SUCCESS) inRunKey = true;
        RegCloseKey(hRunKey);
    }

    // 检查专属配置区记录的路径
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\WinKeyDefender", 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &hAppKey, NULL) == ERROR_SUCCESS) {
        wchar_t buffer[MAX_PATH] = { 0 };
        DWORD size = sizeof(buffer);
        if (RegQueryValueExW(hAppKey, L"AutoStartPath", NULL, NULL, (BYTE*)buffer, &size) == ERROR_SUCCESS) {
            inAppKey = true;
            appSavedPath = buffer;
        }
    }

    // 逻辑 1：被第三方工具删除了自启项，但我们自己还记着 -> 同步清除自身记录
    if (!inRunKey && inAppKey) {
        RegDeleteValueW(hAppKey, L"AutoStartPath");
        inAppKey = false;
    }

    // 逻辑 2：自启项还在，但用户把 exe 文件改名或移动了位置！
    wchar_t currentExePath[MAX_PATH];
    GetModuleFileNameW(NULL, currentExePath, MAX_PATH);

    if (inRunKey && inAppKey && appSavedPath != currentExePath) {
        if (hWnd) {
            int res = MessageBoxW(hWnd, GetStringRes(IDS_CHK_AUTOSTART).c_str(), GetStringRes(IDS_CHK_AUTOSTART_TITLE).c_str(), MB_YESNO | MB_ICONWARNING);
            if (res == IDYES) {
                SetAutoStart(true); // 用当前新路径重新覆盖注册表
                if (hAppKey) RegCloseKey(hAppKey);
                return true;
            } else {
                SetAutoStart(false); // 用户拒绝修复，则彻底清理失效的自启项
                if (hAppKey) RegCloseKey(hAppKey);
                return false;
            }
        }
    }

    if (hAppKey) RegCloseKey(hAppKey);
    return inRunKey;
}

// ==============================================================================
// 注册表持久化机制
// ==============================================================================
void SaveDevicePathToRegistry(const std::wstring& path) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\WinKeyDefender", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"DevicePath", 0, REG_SZ, (const BYTE*)path.c_str(), (DWORD)(path.length() + 1) * static_cast<DWORD>(sizeof(WCHAR)));
        RegCloseKey(hKey);
    }
}

std::wstring LoadDevicePathFromRegistry() {
    HKEY hKey;
    std::wstring path = L"";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\WinKeyDefender", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR buffer[1024] = { 0 };
        DWORD size = sizeof(buffer);
        if (RegQueryValueExW(hKey, L"DevicePath", NULL, NULL, (LPBYTE)buffer, &size) == ERROR_SUCCESS) {
            path = buffer;
        }
        RegCloseKey(hKey);
    }
    return path;
}

// ==============================================================================
// 增加：报警参数的持久化存取
// ==============================================================================
void SaveAlertConfigToRegistry() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\WinKeyDefender", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD timeMs = g_AlertTimeMs.load();
        DWORD count = g_AlertCount.load();
        RegSetValueExW(hKey, L"AlertTimeMs", 0, REG_DWORD, (const BYTE*)&timeMs, sizeof(DWORD));
        RegSetValueExW(hKey, L"AlertCount", 0, REG_DWORD, (const BYTE*)&count, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

void LoadAlertConfigFromRegistry() {
    DWORD timeMs = 1000, count = 5;
    DWORD size = sizeof(DWORD);
    
    // 1. 使用现代 API 原子化读取时间 (RRF_RT_REG_DWORD 强校验类型，绝不占用长期句柄)
    LSTATUS status1 = RegGetValueW(HKEY_CURRENT_USER, L"Software\\WinKeyDefender", L"AlertTimeMs", RRF_RT_REG_DWORD, NULL, &timeMs, &size);
    if (status1 == ERROR_SUCCESS) {
        g_AlertTimeMs = timeMs;
    }

    // 2. 复位 size，原子化读取次数
    size = sizeof(DWORD); 
    LSTATUS status2 = RegGetValueW(HKEY_CURRENT_USER, L"Software\\WinKeyDefender", L"AlertCount", RRF_RT_REG_DWORD, NULL, &count, &size);
    if (status2 == ERROR_SUCCESS) {
        g_AlertCount = count;
    }

    // 3. 完美兜底：只要有任何一个键缺失，或者这是在一台全新的电脑上首次运行，立刻将当前合法的默认值写回注册表补齐
    if (status1 != ERROR_SUCCESS || status2 != ERROR_SUCCESS) {
        SaveAlertConfigToRegistry();
    }
}

// ==============================================================================
// 硬件及互动层
// ==============================================================================
bool HardwareTest(const std::wstring& path, bool lock) {
    HANDLE hWrite = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hWrite == INVALID_HANDLE_VALUE) return false;

    BYTE featBuf[64] = { 0 };
    featBuf[0] = 0x5D; featBuf[1] = 0xBF; featBuf[2] = 0x00; featBuf[3] = lock ? 0x01 : 0x00;
    HidD_SetFeature(hWrite, featBuf, sizeof(featBuf));

    BYTE commitBuf[2] = { 0x01, 0x01 };
    DWORD bytesWritten = 0;
    WriteFile(hWrite, commitBuf, sizeof(commitBuf), &bytesWritten, NULL);
    CloseHandle(hWrite);
    return true;
}

void LogMessage(const std::wstring& msg) {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    std::wstring formattedMsg = msg + L"\r\n";
    if (g_hMainWnd) {
        BSTR bstrMsg = SysAllocString(formattedMsg.c_str());
        PostMessageW(g_hMainWnd, WM_USER_LOG, 0, (LPARAM)bstrMsg);
    }
}

void ShowTrayNotification(const wchar_t* title, const wchar_t* msg, DWORD iconFlag) {
    if (!g_EnableNotify) return;
    wcscpy_s(g_nid.szInfoTitle, title);
    wcscpy_s(g_nid.szInfo, msg);
    g_nid.dwInfoFlags = iconFlag;
    // ⚠️ 修复关键点：必须叠加所有状态，否则 Win11 会把你的托盘事件吞掉
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void SwitchDevice(const std::wstring& path) {
    if (g_hDevice != INVALID_HANDLE_VALUE) { CancelIoEx(g_hDevice, NULL); CloseHandle(g_hDevice); g_hDevice = INVALID_HANDLE_VALUE; }
    g_CurrentDevicePath = path;
    LogMessage(GetStringRes(IDS_LOG_SWITCH_DEVICE).c_str());
}

// 模拟 Python 的排查引擎，恢复所有测试状态
void RestoreAllTestStates() {
    for (auto& d : g_ScannedDevices) {
        if (d.state == STATE_TESTING) {
            HardwareTest(d.path, false);
            d.state = STATE_INVALID;
        }
    }
}

// 发起测试 (发送锁定 Payload)
void StartInteractiveTest(int index) {
    RestoreAllTestStates();
    g_ScannedDevices[index].state = STATE_TESTING;
    HardwareTest(g_ScannedDevices[index].path, true); // 发送锁定测试
    ListView_RedrawItems(hListDevices, 0, g_ScannedDevices.size());
    LogMessage(GetStringRes(IDS_LOG_INTERACTIVE_TIP_P1).c_str() + std::to_wstring(index + 1) + GetStringRes(IDS_LOG_INTERACTIVE_TIP_P2).c_str());
}

// 结束测试判断
void EndInteractiveTest(int index, bool isValid) {
    HardwareTest(g_ScannedDevices[index].path, false); // 不管对错，先给人家解开

    if (isValid) {
        g_ScannedDevices[index].state = STATE_VALID;
        LogMessage(GetStringRes(IDS_LOG_INTERACTIVE_SUCCESS).c_str());
        SaveDevicePathToRegistry(g_ScannedDevices[index].path);
        SetWindowTextW(hEditCustom, g_ScannedDevices[index].path.c_str());
        SwitchDevice(g_ScannedDevices[index].path);
        ListView_RedrawItems(hListDevices, 0, g_ScannedDevices.size());
    }
    else {
        g_ScannedDevices[index].state = STATE_INVALID;
        // 查找下一个未测的行，实现自动循环跳跃
        int nextIndex = (index + 1) % g_ScannedDevices.size();
        int startCheck = nextIndex;
        bool found = false;
        do {
            if (g_ScannedDevices[nextIndex].state != STATE_VALID && g_ScannedDevices[nextIndex].state != STATE_INVALID) {
                found = true; break;
            }
            nextIndex = (nextIndex + 1) % g_ScannedDevices.size();
        } while (nextIndex != startCheck);

        ListView_RedrawItems(hListDevices, 0, g_ScannedDevices.size());
        if (found) StartInteractiveTest(nextIndex);
        else LogMessage(GetStringRes(IDS_LOG_INTERACTIVE_FAILED).c_str());
    }
}

// 后台异步扫描，完全复刻 Python 的枚举判断逻辑
void AsyncEnumerateThread() {
    std::vector<ScannedDevice> devs;
    GUID hidGuid; HidD_GetHidGuid(&hidGuid);
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (hDevInfo != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA devInfoData; devInfoData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        DWORD devIndex = 0;
        while (SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &hidGuid, devIndex, &devInfoData)) {
            DWORD reqSize = 0;
            SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devInfoData, NULL, 0, &reqSize, NULL);
            auto detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
            detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devInfoData, detailData, reqSize, NULL, NULL)) {
                std::wstring path = detailData->DevicePath;
                // 1. 特征一：匹配 Asus VID 和嫌疑 PID (18C6 / 19B6)
                if (path.find(L"vid_0b05") != std::wstring::npos && (path.find(L"pid_18c6") != std::wstring::npos || path.find(L"pid_19b6") != std::wstring::npos)) {

                    // 2. 特征二：Usage Page >= 0xFF00 (深度嗅探)
                    HANDLE hFile = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                    if (hFile != INVALID_HANDLE_VALUE) {
                        PHIDP_PREPARSED_DATA ppData = NULL;
                        if (HidD_GetPreparsedData(hFile, &ppData)) {
                            HIDP_CAPS caps;
                            if (HidP_GetCaps(ppData, &caps) == HIDP_STATUS_SUCCESS) {
                                if (caps.UsagePage >= 0xFF00) {
                                    ScannedDevice dev; dev.path = path; dev.state = STATE_UNTESTED;
                                    devs.push_back(dev);
                                }
                            }
                            HidD_FreePreparsedData(ppData);
                        }
                        CloseHandle(hFile);
                    }
                }
            }
            free(detailData); devIndex++;
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }
    auto pDevs = new std::vector<ScannedDevice>(std::move(devs));
    PostMessageW(g_hMainWnd, WM_USER_ENUM_DONE, (WPARAM)pDevs, 0);
}

// 核心读写监听线程 (保持不变)
void HIDMonitorThreadWorker() {
    BYTE readBuf[64] = { 0 }, featBuf[64] = { 0 }, checkBuf[64] = { 0 }, commitBuf[2] = { 0x01, 0x01 };
    std::deque<std::chrono::steady_clock::time_point> lock_timestamps;
    auto fail_start_time = std::chrono::steady_clock::now();
    bool in_fail_state = false, manual_alert_sent = false;

    while (g_Running) {
        if (g_CurrentDevicePath.empty()) { Sleep(500); continue; }
        if (g_hDevice == INVALID_HANDLE_VALUE) {
            g_hDevice = CreateFileW(g_CurrentDevicePath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (g_hDevice == INVALID_HANDLE_VALUE) { Sleep(2000); continue; }
        }

        DWORD bytesRead = 0;
        if (ReadFile(g_hDevice, readBuf, sizeof(readBuf), &bytesRead, NULL)) {
            if (bytesRead >= 4 && readBuf[0] == 0x5D && readBuf[1] == 0xBF && readBuf[2] == 0x01) {
                if (readBuf[3] == 0x01) {
                    g_IsLocked = true;
                    PostMessageW(g_hMainWnd, WM_USER_NOTIFY, 0, 0); // 👇 新增：发个空通知触发图标变红叉
                    auto now = std::chrono::steady_clock::now();
                    LogMessage(GetStringRes(IDS_LOG_KEY_LOCK_DETECTED).c_str());

                    if (!g_ActiveDefense) {
                        // 加上这个 if 限制，只有开启了报警复选框才执行记录和弹窗
                        if (g_EnableAlert) {
                            lock_timestamps.push_back(now);
                            while (!lock_timestamps.empty()) {
                                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lock_timestamps.front()).count();
                                if (elapsed > g_AlertTimeMs) lock_timestamps.pop_front();
                                else break;
                            }
                            if (lock_timestamps.size() >= g_AlertCount) {
                                if (g_EnableNotify) {
                                    wcscpy_s(g_nid.szInfoTitle, GetStringRes(IDS_NOTIF_LOCKED_TITLE).c_str()); wcscpy_s(g_nid.szInfo, GetStringRes(IDS_NOTIF_LOCKED_DETAIL).c_str());
                                    g_nid.dwInfoFlags = NIIF_WARNING; g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO; Shell_NotifyIconW(NIM_MODIFY, &g_nid);
                                }
                            LogMessage(GetStringRes(IDS_LOG_KEY_LOCK_ALERT).c_str()); lock_timestamps.clear();
                            }
                        }
                        continue;
                    }

                    LogMessage(GetStringRes(IDS_LOG_KEY_ATTEMPT_AUTOUNLOCK).c_str());
                    ZeroMemory(featBuf, sizeof(featBuf));
                    featBuf[0] = 0x5D; featBuf[1] = 0xBF; featBuf[2] = 0x00; featBuf[3] = 0x00;
                    HidD_SetFeature(g_hDevice, featBuf, sizeof(featBuf));
                    DWORD bytesWritten = 0; WriteFile(g_hDevice, commitBuf, sizeof(commitBuf), &bytesWritten, NULL);
                    Sleep(100);

                    ZeroMemory(checkBuf, sizeof(checkBuf)); checkBuf[0] = 0x5D;
                    if (HidD_GetFeature(g_hDevice, checkBuf, sizeof(checkBuf))) {
                        if (checkBuf[3] == 0x00) {
                            LogMessage(GetStringRes(IDS_LOG_KEY_AUTOUNLOCK_SUCCESS).c_str());
                            g_IsLocked = false; in_fail_state = false; manual_alert_sent = false;
                            PostMessageW(g_hMainWnd, WM_USER_NOTIFY, NOTIFY_SUCCESS, 0);
                        }
                        else if (checkBuf[3] == 0x01) {
                            if (!in_fail_state) { in_fail_state = true; fail_start_time = std::chrono::steady_clock::now(); }
                            else if (!manual_alert_sent) {
                                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - fail_start_time).count();
                                if (elapsed >= 1000) {
								    PostMessageW(g_hMainWnd, WM_USER_NOTIFY, NOTIFY_ERROR, 0);
								    LogMessage(GetStringRes(IDS_LOG_KEY_AUTOUNLOCK_FAILED).c_str());
								    manual_alert_sent = true;
                                }
                            }
                        }
                    }
                }
                else if (readBuf[3] == 0x00) {
                    g_IsLocked = false;
                    PostMessageW(g_hMainWnd, WM_USER_NOTIFY, 0, 0); // 👇 新增：发个空通知触发图标恢复
                    LogMessage(GetStringRes(IDS_LOG_KEY_UNLOCK).c_str());
                }
            }
        }
        else {
            CloseHandle(g_hDevice); g_hDevice = INVALID_HANDLE_VALUE; LogMessage(GetStringRes(IDS_LOG_DEVICE_DISCONN).c_str());
        }
    }
}

// ==============================================================================
// 界面主题与子类化修复引擎
// ==============================================================================
bool IsSystemDarkMode() {
    DWORD useLightTheme = 1; DWORD dataSize = sizeof(DWORD); HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&useLightTheme, &dataSize); RegCloseKey(hKey);
    }
    return useLightTheme == 0;
}
void EnableGlobalAppDarkMode() {
    HMODULE hUxtheme = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hUxtheme) {
        using fnSetPreferredAppMode = int(WINAPI*)(int); auto SetAppMode = (fnSetPreferredAppMode)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135));
        if (SetAppMode) SetAppMode(1);
        using fnFlushMenuThemes = void(WINAPI*)(); auto FlushMenu = (fnFlushMenuThemes)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(136));
        if (FlushMenu) FlushMenu();
        FreeLibrary(hUxtheme);
    }
}

LRESULT CALLBACK ListViewSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    if (uMsg == WM_NOTIFY) {
        LPNMHDR pnmh = (LPNMHDR)lParam;
        if (pnmh->hwndFrom == (HWND)SendMessage(hWnd, LVM_GETHEADER, 0, 0) && pnmh->code == NM_CUSTOMDRAW) {
            LPNMCUSTOMDRAW lpnmcd = (LPNMCUSTOMDRAW)lParam;
            if (lpnmcd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (lpnmcd->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(lpnmcd->hdc, g_TextColor); return CDRF_NEWFONT;
            }
        }
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void ApplyTheme() {
    g_IsDarkMode = IsSystemDarkMode();
    g_BgColor = g_IsDarkMode ? RGB(32, 32, 32) : RGB(240, 240, 240);
    g_TextColor = g_IsDarkMode ? RGB(220, 220, 220) : RGB(0, 0, 0);
    COLORREF editBgColor = g_IsDarkMode ? RGB(45, 45, 45) : RGB(255, 255, 255);

    if (g_hBgBrush) DeleteObject(g_hBgBrush); if (g_hEditBgBrush) DeleteObject(g_hEditBgBrush);
    g_hBgBrush = CreateSolidBrush(g_BgColor); g_hEditBgBrush = CreateSolidBrush(editBgColor);

    BOOL darkVal = g_IsDarkMode ? TRUE : FALSE;
    if (g_hMainWnd) DwmSetWindowAttribute(g_hMainWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkVal, sizeof(darkVal));
    if (g_hSettingsWnd) DwmSetWindowAttribute(g_hSettingsWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkVal, sizeof(darkVal));

    LPCWSTR themeName = g_IsDarkMode ? L"DarkMode_Explorer" : L"Explorer";
    LPCWSTR itemsTheme = g_IsDarkMode ? L"DarkMode_ItemsView" : L"ItemsView";

    if (hListDevices) {
        SetWindowTheme(hListDevices, themeName, NULL);
        ListView_SetBkColor(hListDevices, g_IsDarkMode ? RGB(32, 32, 32) : RGB(255, 255, 255));
        ListView_SetTextBkColor(hListDevices, g_IsDarkMode ? RGB(32, 32, 32) : RGB(255, 255, 255));
        ListView_SetTextColor(hListDevices, g_TextColor);
        SetWindowTheme((HWND)SendMessage(hListDevices, LVM_GETHEADER, 0, 0), itemsTheme, NULL);
    }

    HWND btns[] = { hBtnEnum, hBtnCustomApply, hLogEdit };
    for (HWND btn : btns) { if (btn) SetWindowTheme(btn, themeName, NULL); }

    // 强制接管 Tooltip 的深色模式
    if (g_hMenuToolTip) {
        SetWindowTheme(g_hMenuToolTip, g_IsDarkMode ? L"DarkMode_Explorer" : L"Explorer", NULL);
        SendMessage(g_hMenuToolTip, TTM_SETTIPBKCOLOR, (WPARAM)g_BgColor, 0);
        SendMessage(g_hMenuToolTip, TTM_SETTIPTEXTCOLOR, (WPARAM)g_TextColor, 0);
    }

    if (g_hMainWnd) InvalidateRect(g_hMainWnd, NULL, TRUE);
    if (g_hSettingsWnd) InvalidateRect(g_hSettingsWnd, NULL, TRUE);
}

int DPIScale(HWND hWnd, int value) { return MulDiv(value, GetDpiForWindow(hWnd), 96); }
// 👇 魔法 1：双缓冲完美拦截重绘，解决高频 Hover 闪烁问题
LRESULT CALLBACK CheckboxTextFixProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    switch (uMsg) {
        case WM_ERASEBKGND:
            // 1. 拦截背景擦除信号。这是消灭闪烁的第一步，直接返回 1 告诉系统“我处理过了”。
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rc;
            GetClientRect(hWnd, &rc);

            // --- 开启双缓冲 (Double Buffering) 核心 ---
            // 创建一块与当前 Checkbox 尺寸相同的“内存隐藏画布”
            HDC hMemDC = CreateCompatibleDC(hdc);
            HBITMAP hMemBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HGDIOBJ hOldBmp = SelectObject(hMemDC, hMemBmp);

            // 2. 先用主界面的背景色铺底，防止圆角边缘出现黑色毛边或残影
            FillRect(hMemDC, &rc, g_hBgBrush);

            // 3. 欺骗原生控件：通过 wParam 将我们的内存画布强塞给原生 Checkbox
            // 在启用了 Comctl32 v6 (Visual Styles) 的情况下，WC_BUTTON 原生支持将画面直接输出到指定的 HDC
            DefSubclassProc(hWnd, WM_PAINT, (WPARAM)hMemDC, 0);

            // 4. 在内存画布上无缝叠加我们的自定义文字
            std::wstring* pText = (std::wstring*)dwRefData;
            if (pText && !pText->empty()) {
                HFONT hFont = (HFONT)SendMessage(hWnd, WM_GETFONT, 0, 0);
                HGDIOBJ hOldFont = SelectObject(hMemDC, hFont);

                SetBkMode(hMemDC, TRANSPARENT);
                SetTextColor(hMemDC, g_TextColor); // 跟随全局的深/浅色模式变量

                RECT textRc = rc;
                // 将文字起始点向右偏移，避开原生的方块 (20 像素配合 DPI 缩放)
                textRc.left += DPIScale(hWnd, 20);

                DrawTextW(hMemDC, pText->c_str(), -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                SelectObject(hMemDC, hOldFont);
            }

            // 5. 原子级绘制：将内存画布合成好的完整图像，一次性 BitBlt 拍到真实的屏幕画布上！(绝对零闪烁)
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, hMemDC, 0, 0, SRCCOPY);

            // --- 清理 GDI 资源，防止内存泄漏 ---
            SelectObject(hMemDC, hOldBmp);
            DeleteObject(hMemBmp);
            DeleteDC(hMemDC);

            EndPaint(hWnd, &ps);
            return 0; // 返回 0 表示我们已经接管并完成了所有的绘制工作
        }

        case WM_NCDESTROY: {
            // 窗口销毁时释放字符串内存，防泄漏
            std::wstring* pText = (std::wstring*)dwRefData;
            if (pText) delete pText;
            RemoveWindowSubclass(hWnd, CheckboxTextFixProc, uIdSubclass);
            break;
        }
    }

    // 其他不涉及重绘的消息，正常放行
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// 👇 魔法 2：极其清爽的创建工厂函数
HWND CreateDarkCheckbox(HWND hParent, const std::wstring& text, int x, int y, int width, int height, int id) {
    // 创建一个完全没有文字的空壳 Checkbox
    HWND hChk = CreateWindowW(WC_BUTTON, L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, x, y, width, height, hParent, (HMENU)(UINT_PTR)id, hInst, NULL);
    // 把文字挂载在内存里，塞给子类化函数
    SetWindowSubclass(hChk, CheckboxTextFixProc, id, (DWORD_PTR)new std::wstring(text));
    return hChk;
}

void AutoSizeListViewColumns(HWND hListView) {
    int colCount = Header_GetItemCount(ListView_GetHeader(hListView));
    for (int i = 0; i < colCount; i++) {
        ListView_SetColumnWidth(hListView, i, LVSCW_AUTOSIZE);
        int w1 = ListView_GetColumnWidth(hListView, i);
        ListView_SetColumnWidth(hListView, i, LVSCW_AUTOSIZE_USEHEADER);
        int w2 = ListView_GetColumnWidth(hListView, i);
        int finalWidth = max(max(w1, w2), DPIScale(hListView, 80));
        ListView_SetColumnWidth(hListView, i, finalWidth);
    }
}
// 新增：根据当前 DPI 和字体动态计算文本的物理宽度
int GetScaledTextWidth(HWND hWnd, HFONT hFont, const std::wstring& text) {
    HDC hdc = GetDC(hWnd);
    HGDIOBJ hOld = SelectObject(hdc, hFont);
    SIZE sz;
    GetTextExtentPoint32W(hdc, text.c_str(), text.length(), &sz);
    SelectObject(hdc, hOld);
    ReleaseDC(hWnd, hdc);
    // 返回文字的纯宽度 + Checkbox 控件本身的方块及间距（约 30px 并支持 DPI 缩放）
    return sz.cx + MulDiv(30, GetDpiForWindow(hWnd), 96);
}

// 新增：计算按钮需要的物理宽度（包含左右边距填充）
int GetScaledButtonWidth(HWND hWnd, HFONT hFont, const std::wstring& text) {
    HDC hdc = GetDC(hWnd);
    HGDIOBJ hOld = SelectObject(hdc, hFont);
    SIZE sz;
    GetTextExtentPoint32W(hdc, text.c_str(), text.length(), &sz);
    SelectObject(hdc, hOld);
    ReleaseDC(hWnd, hdc);
    // 纯文字宽度 + 左右各 15px 的 Padding (合计 30px 并支持 DPI 缩放)
    return sz.cx + DPIScale(hWnd, 30);
}

// 新增：计算纯文本标签(Static)的物理宽度
int GetScaledLabelWidth(HWND hWnd, HFONT hFont, const std::wstring& text) {
    HDC hdc = GetDC(hWnd);
    HGDIOBJ hOld = SelectObject(hdc, hFont);
    SIZE sz;
    GetTextExtentPoint32W(hdc, text.c_str(), text.length(), &sz);
    SelectObject(hdc, hOld);
    ReleaseDC(hWnd, hdc);
    // 纯文字宽度 + 5px 安全边距防裁剪
    return sz.cx + DPIScale(hWnd, 5);
}

void UpdateWindowFont(HWND hWnd, HFONT& hFont) {
    if (hFont) DeleteObject(hFont);

    // 获取当前操作系统首选的标准界面字体度量信息
    NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &ncm, 0);

    // 强制按高分屏重写文字高度，保留系统原生字形（完全规避字体不存在的风险）
    int fontSize = MulDiv(10, GetDpiForWindow(hWnd), 72);
    ncm.lfMessageFont.lfHeight = -fontSize;

    hFont = CreateFontIndirectW(&ncm.lfMessageFont);
    EnumChildWindows(hWnd, [](HWND child, LPARAM font) -> BOOL { SendMessage(child, WM_SETFONT, font, TRUE); return TRUE; }, (LPARAM)hFont);
}

// 新增：利用 GPU 图层透明度实现的高帧率丝滑淡入
// 终极完美版：GPU 透明度平滑淡入 + 绝对前台抢占
void SmoothFadeInWindow(HWND hWnd) {
    LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hWnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);

    // 1. 设置透明度为 0，然后解除隐藏状态 (此时窗口完全透明)
    SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);
    ShowWindow(hWnd, SW_RESTORE);

    // 👇 核心修复 1：在动画开始前，强行将完全透明的窗口拉到最顶层并抢占焦点！
    // 因为这是由用户直接点击托盘触发的，系统会毫不犹豫地赋予前台权限
    SetForegroundWindow(hWnd);
    SetActiveWindow(hWnd);
    SetWindowPos(hWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    // 2. 强制渲染第一帧
    RedrawWindow(hWnd, NULL, NULL, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);

    // 3. 高帧率平滑过渡循环
    for (int i = 0; i <= 255; i += 26) {
        SetLayeredWindowAttributes(hWnd, 0, (BYTE)i, LWA_ALPHA);

        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Sleep(10);
    }

    // 4. 收尾工作
    SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
    SetWindowLongPtrW(hWnd, GWL_EXSTYLE, exStyle);
    RedrawWindow(hWnd, NULL, NULL, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

// ==============================================================================
// 窗口过程
// ==============================================================================
// 关于窗口子控件动态重排布局引擎
void RearrangeSettingsLayout(HWND hWnd, int dpi) {
    auto Scale = [dpi](int val) { return MulDiv(val, dpi, 96); };

    // 1. 排布左侧的 Logo 图标
    // X=20, Y=20, 宽度48, 高度48 (必须与你在 LoadImageW 填写的尺寸一致)
    if (hSetImgLogo) MoveWindow(hSetImgLogo, Scale(20), Scale(20), Scale(48), Scale(48), TRUE);

    // 2. 排布右侧的文本信息
    // 统一设置新的 X 坐标为 85，为左侧图标留出足够的空间
    int textX = Scale(85);

    if (hSetLblTitle) MoveWindow(hSetLblTitle, textX, Scale(20), Scale(300), Scale(25), TRUE);
    if (hSetLblAuthor) MoveWindow(hSetLblAuthor, textX, Scale(50), Scale(300), Scale(25), TRUE);
    if (hSetLblGit) MoveWindow(hSetLblGit, textX, Scale(80), Scale(200), Scale(25), TRUE);
}

LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
    {
        // 👇 新增：强制为当前“关于/设置”窗口换上专属图标
        HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1));
        SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);   // 修复任务栏和 Alt+Tab 看到的大图标
        SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon); // 修复窗口左上角的小图标

        // 👇 新增：在设置窗口创建的第一时间，立即同步当前的深色模式状态
        BOOL darkVal = g_IsDarkMode ? TRUE : FALSE;
        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkVal, sizeof(darkVal));

        // 👇 新增：创建 Logo 图像控件
        // 注意样式使用 SS_ICON，不需要文本
        hSetImgLogo = CreateWindowW(WC_STATIC, NULL, WS_CHILD | WS_VISIBLE | SS_ICON, 0, 0, 0, 0, hWnd, NULL, hInst, NULL);

        // 👇 新增：加载你之前导入的图标，并发送给控件
        // 这里以 48x48 大小为例，你可以按需改成 64x64 或其他尺寸
        // 使用 LR_SHARED 标志，系统会自动管理图标内存，防止泄漏
        HANDLE hLogoIcon = LoadImageW(hInst, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR | LR_SHARED);
        SendMessageW(hSetImgLogo, STM_SETICON, (WPARAM)hLogoIcon, 0);

        // 创建控件时不写死物理坐标，交由下方的重排引擎统一处理
        hSetLblTitle = CreateWindowW(WC_STATIC, GetStringRes(IDS_LBL_ABOUT_PRODUCT).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, NULL, hInst, NULL);
        hSetLblAuthor = CreateWindowW(WC_STATIC, GetStringRes(IDS_LBL_ABOUT_AUTHOR).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd, NULL, hInst, NULL);
        hSetLblGit = CreateWindowW(WC_STATIC, GetStringRes(IDS_LBL_ABOUT_GITHUB).c_str(), WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_LEFT, 0, 0, 0, 0, hWnd, (HMENU)ID_LINK_GITHUB, hInst, NULL);

        // 应用初始 DPI 字体与布局
        UpdateWindowFont(hWnd, g_hFontSettings);
        RearrangeSettingsLayout(hWnd, GetDpiForWindow(hWnd));
        break;
    }
    case WM_DPICHANGED: {
        // 当关于窗口被单独拖动到另一个不同 DPI 的显示器时触发
        int newDpi = LOWORD(wParam);
        RECT* prcNewWindow = (RECT*)lParam;

        // 缩放关于窗口外框
        SetWindowPos(hWnd, NULL, prcNewWindow->left, prcNewWindow->top, prcNewWindow->right - prcNewWindow->left, prcNewWindow->bottom - prcNewWindow->top, SWP_NOZORDER | SWP_NOACTIVATE);

        // 刷新关于窗口内部字体与控件布局
        UpdateWindowFont(hWnd, g_hFontSettings);
        RearrangeSettingsLayout(hWnd, newDpi);

        // 👇 加上这一行：强制擦除旧背景并全量重绘，残影瞬间消失
        InvalidateRect(hWnd, NULL, TRUE);
        break;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, g_hBgBrush);
        return 1;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_LINK_GITHUB) {
            ShellExecuteW(NULL, L"open", L"https://github.com/JoeYe-233/AsusWinKeyActiveGuard", NULL, NULL, SW_SHOWNORMAL);
        }
        break;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND hCtrl = (HWND)lParam;
        SetBkMode(hdc, TRANSPARENT);
        if (GetDlgCtrlID(hCtrl) == ID_LINK_GITHUB) {
            SetTextColor(hdc, g_IsDarkMode ? RGB(100, 150, 255) : RGB(0, 50, 200));
        } else {
            SetTextColor(hdc, g_TextColor);
        }
        return (INT_PTR)g_hBgBrush;
    }

    case WM_SETCURSOR:
        if (GetDlgCtrlID((HWND)wParam) == ID_LINK_GITHUB) {
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }
        return DefWindowProc(hWnd, message, wParam, lParam);

    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

void UpdateTrayIcon() {
    // 锁定状态拥有最高视觉优先级：显示红叉 (IDI_ERROR)
    if (g_IsLocked) {
        g_nid.hIcon = LoadIcon(NULL, IDI_ERROR);
        wcscpy_s(g_nid.szTip, GetStringRes(IDS_TRAYICON_TIP_LOCKED).c_str());
    }
    // 未锁定状态：根据防御开关显示 盾牌(IDI_SHIELD) 或 警告(IDI_WARNING)
    else {
        g_nid.hIcon = LoadIcon(NULL, g_ActiveDefense ? IDI_SHIELD : IDI_WARNING);
        wcscpy_s(g_nid.szTip, g_ActiveDefense ? GetStringRes(IDS_TRAYICON_TIP_ACTIVE).c_str() : GetStringRes(IDS_TRAYICON_TIP_IDLE).c_str());
    }

    // 👇 核心修复：加上 NIF_SHOWTIP 强制穿透气泡通知的屏蔽！
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
void RearrangeControlsLayout(HWND hWnd, int dpi) {
    // 建立基于当前屏幕实际 DPI 的绝对像素缩放闭包
    auto Scale = [dpi](int val) { return MulDiv(val, dpi, 96); };

    // 1. 获取当前主窗体专属字体的度量上下文
    HDC hdc = GetDC(hWnd);
    SelectObject(hdc, g_hFontMain);
    SIZE sz;

    // 2. 动态收集由于模板流水线生成、没有全局变量句柄的 Static 控件
    // 在 Win32 底层系统 Z-Order 中，它们是在 hChkAlert 之后按顺序连续创建的
    std::vector<HWND> dynamicStatics;
    HWND hChild = GetWindow(hWnd, GW_CHILD);
    bool startCollect = false;
    while (hChild != NULL) {
        if (hChild == hChkAlert) {
            startCollect = true;
        }
        if (startCollect && hChild != hChkAlert && hChild != hEditTime && hChild != hEditCount && hChild != hLogEdit) {
            wchar_t className[64] = { 0 };
            GetClassNameW(hChild, className, 64);
            if (wcscmp(className, L"Static") == 0) {
                dynamicStatics.push_back(hChild);
            }
        }
        hChild = GetWindow(hChild, GW_HWNDNEXT);
    }

    // 3. 分配延迟窗口位置事务句柄
    // 包含：1(列表) + 1(标签) + 1(编辑框) + 2(按钮) + 2(固定复选框) + 1(报警选框) + 2(报警编辑框) + 2(模板文本) + 1(日志框) = 13 个窗口
    HDWP hdwp = BeginDeferWindowPos(14);
    if (!hdwp) {
        ReleaseDC(hWnd, hdc);
        return;
    }

    // --- 第一排：设备总线列表 ---
    hdwp = DeferWindowPos(hdwp, hListDevices, NULL, Scale(10), Scale(10), Scale(660), Scale(150), SWP_NOZORDER | SWP_NOACTIVATE);

    // --- 第二排：自定义 ID 输入与功能控制区 ---
    // 1. 右侧推算：确定整体右侧绝对边界
    int rightBound = Scale(670);
    int wApply = GetScaledButtonWidth(hWnd, g_hFontMain, GetStringRes(IDS_BUTTON_APPLY));
    int wEnum = GetScaledButtonWidth(hWnd, g_hFontMain, GetStringRes(IDS_BUTTON_ENUM));

    int xApply = rightBound - wApply;
    int xEnum = xApply - Scale(10) - wEnum;

    // 2. 左侧推算：计算 Label 宽度并向右推导 Edit 的起点
    int xLabel = Scale(10);
    int wLabel = GetScaledLabelWidth(hWnd, g_hFontMain, GetStringRes(IDS_LBL_CUSTOM_ID));
    // Edit 框的左边缘 = Label 左边缘 + Label 宽度 + 10px 间距
    int xEdit = xLabel + wLabel + Scale(10);

    // 3. 中间弹性拉伸：计算剩余空间给 Edit 框
    int wEdit = max(Scale(50), xEnum - Scale(10) - xEdit);

    // 4. 提交位置 (替换掉原先的硬编码)
    hdwp = DeferWindowPos(hdwp, hLblCustom, NULL, xLabel, Scale(175), wLabel, Scale(25), SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, hEditCustom, NULL, xEdit, Scale(170), wEdit, Scale(25), SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, hBtnEnum, NULL, xEnum, Scale(170), wEnum, Scale(25), SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, hBtnCustomApply, NULL, xApply, Scale(170), wApply, Scale(25), SWP_NOZORDER | SWP_NOACTIVATE);

    // --- 第三排：主动防御复选框 ---
    int wDefense = GetScaledTextWidth(hWnd, g_hFontMain, GetStringRes(IDS_LBL_ACTIVE_DEFENCE));
    hdwp = DeferWindowPos(hdwp, hChkDefense, NULL, Scale(10), Scale(210), wDefense, Scale(25), SWP_NOZORDER | SWP_NOACTIVATE);

    // 👇 新增：第四排：开机自启
    int wStartup = GetScaledTextWidth(hWnd, g_hFontMain, GetStringRes(IDS_LBL_AUTO_START));
    hdwp = DeferWindowPos(hdwp, hChkStartup, NULL, Scale(10), Scale(240), wStartup, Scale(25), SWP_NOZORDER | SWP_NOACTIVATE);

    // --- 第五排：托盘气泡通知 (Y 坐标从 240 变为 270) ---
    int wNotify = GetScaledTextWidth(hWnd, g_hFontMain, GetStringRes(IDS_LBL_TRAY_BALLOON));
    hdwp = DeferWindowPos(hdwp, hChkNotify, NULL, Scale(10), Scale(270), wNotify, Scale(25), SWP_NOZORDER | SWP_NOACTIVATE);

    // --- 第六排：i18n 模板流水线 (Y 坐标从 270 变为 300) ---
    int curX = Scale(10);
    int curY = Scale(300);

    std::wstring tpl = GetStringRes(IDS_LBL_LOCK_NOTIF).c_str();
    std::wstring curStr = L"";
    bool isFirst = true;
    size_t staticIdx = 0;

    for (size_t i = 0; i < tpl.length(); ) {
        if (tpl.substr(i, 6) == L"{Time}") {
            if (!curStr.empty()) {
                GetTextExtentPoint32W(hdc, curStr.c_str(), curStr.length(), &sz);
                if (isFirst) {
                    int w = sz.cx + Scale(25);
                    hdwp = DeferWindowPos(hdwp, hChkAlert, NULL, curX, curY, w, Scale(25), SWP_NOZORDER | SWP_NOACTIVATE);
                    curX += w + Scale(2);
                    isFirst = false;
                } else {
                    if (staticIdx < dynamicStatics.size()) {
                        hdwp = DeferWindowPos(hdwp, dynamicStatics[staticIdx], NULL, curX, curY + Scale(3), sz.cx, Scale(20), SWP_NOZORDER | SWP_NOACTIVATE);
                        staticIdx++;
                    }
                    curX += sz.cx + Scale(2);
                }
                curStr = L"";
            }
            hdwp = DeferWindowPos(hdwp, hEditTime, NULL, curX, curY, Scale(50), Scale(25), SWP_NOZORDER | SWP_NOACTIVATE);
            curX += Scale(52);
            i += 6;
        }
        else if (tpl.substr(i, 7) == L"{Count}") {
            if (!curStr.empty()) {
                GetTextExtentPoint32W(hdc, curStr.c_str(), curStr.length(), &sz);
                if (isFirst) {
                    int w = sz.cx + Scale(25);
                    hdwp = DeferWindowPos(hdwp, hChkAlert, NULL, curX, curY, w, Scale(25), SWP_NOZORDER | SWP_NOACTIVATE);
                    curX += w + Scale(2);
                    isFirst = false;
                } else {
                    if (staticIdx < dynamicStatics.size()) {
                        hdwp = DeferWindowPos(hdwp, dynamicStatics[staticIdx], NULL, curX, curY + Scale(3), sz.cx, Scale(20), SWP_NOZORDER | SWP_NOACTIVATE);
                        staticIdx++;
                    }
                    curX += sz.cx + Scale(2);
                }
                curStr = L"";
            }
            hdwp = DeferWindowPos(hdwp, hEditCount, NULL, curX, curY, Scale(30), Scale(25), SWP_NOZORDER | SWP_NOACTIVATE);
            curX += Scale(32);
            i += 7;
        }
        else {
            curStr += tpl[i];
            i++;
        }
    }
    if (!curStr.empty()) {
        GetTextExtentPoint32W(hdc, curStr.c_str(), curStr.length(), &sz);
        if (staticIdx < dynamicStatics.size()) {
            hdwp = DeferWindowPos(hdwp, dynamicStatics[staticIdx], NULL, curX, curY + Scale(3), sz.cx, Scale(20), SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // --- 第七排：底层监控日志输出底板 (Y 坐标从 300 变为 330，高度从 240 压缩为 210) ---
    hdwp = DeferWindowPos(hdwp, hLogEdit, NULL, Scale(10), Scale(330), Scale(660), Scale(240), SWP_NOZORDER | SWP_NOACTIVATE);

    // 4. 原子级一次性提交所有控件变动，由底层硬件进行统一合并绘制
    EndDeferWindowPos(hdwp);
    ReleaseDC(hWnd, hdc);
}
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_bIsInitializing = true;
        // 👇 新增：利用当前绝对有效的局部 hWnd，在创建的第一帧就注入深色标题栏
        BOOL darkVal = IsSystemDarkMode() ? TRUE : FALSE;
        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkVal, sizeof(darkVal));

        // 👇 将字体的初始化提前到最前面，并对接系统的默认 UI 字体（满足需求 2）
        if (!g_hFontMain) {
            NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &ncm, 0);
            int fontSize = MulDiv(10, GetDpiForWindow(hWnd), 72);
            ncm.lfMessageFont.lfHeight = -fontSize;
            g_hFontMain = CreateFontIndirectW(&ncm.lfMessageFont);
        }

        // --- 核心控件搭建 (全量支持 HiDPI 缩放) ---
        hListDevices = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL, DPIScale(hWnd, 10), DPIScale(hWnd, 10), DPIScale(hWnd, 660), DPIScale(hWnd, 150), hWnd, (HMENU)1000, hInst, NULL);
        ListView_SetExtendedListViewStyle(hListDevices, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        SetWindowSubclass(hListDevices, ListViewSubclassProc, 0, 0);

        // 1. 先用局部变量保存字符串，确保它们的生命周期覆盖整个插入过程
        std::wstring strStatus = GetStringRes(IDS_LISTBOX_COL_STATUS);
        std::wstring strDeviceId = GetStringRes(IDS_LISTBOX_COL_DEVICE_ID);

        // 2. 建议显式清零结构体，这是 Win32 开发的好习惯，避免未初始化的字段引发奇葩问题
        LVCOLUMN lvc = { 0 };
        lvc.mask = LVCF_TEXT | LVCF_WIDTH;
        lvc.fmt = 0; // 默认左对齐

        // 3. 插入第 0 列 (状态)
        lvc.cx = DPIScale(hWnd, 100);
        lvc.pszText = (LPWSTR)strStatus.c_str();
        ListView_InsertColumn(hListDevices, 0, &lvc);

        // 4. 插入第 1 列 (路径/设备ID)
        lvc.cx = DPIScale(hWnd, 540);
        lvc.pszText = (LPWSTR)strDeviceId.c_str();
        ListView_InsertColumn(hListDevices, 1, &lvc);

        // --- 提取：第二排功能控制区自适应布局 ---
        int rightBound = DPIScale(hWnd, 670);
        int wApply = GetScaledButtonWidth(hWnd, g_hFontMain, GetStringRes(IDS_BUTTON_APPLY));
        int wEnum = GetScaledButtonWidth(hWnd, g_hFontMain, GetStringRes(IDS_BUTTON_ENUM));

        int xApply = rightBound - wApply;
        int xEnum = xApply - DPIScale(hWnd, 10) - wEnum;

        // 左侧推算
        int xLabel = DPIScale(hWnd, 10);
        int wLabel = GetScaledLabelWidth(hWnd, g_hFontMain, GetStringRes(IDS_LBL_CUSTOM_ID));
        int xEdit = xLabel + wLabel + DPIScale(hWnd, 10);

        // 中间拉伸
        int wEdit = max(DPIScale(hWnd, 50), xEnum - DPIScale(hWnd, 10) - xEdit);

        // 创建控件
        hLblCustom = CreateWindowW(WC_STATIC, GetStringRes(IDS_LBL_CUSTOM_ID).c_str(), WS_CHILD | WS_VISIBLE, xLabel, DPIScale(hWnd, 175), wLabel, DPIScale(hWnd, 25), hWnd, NULL, hInst, NULL);
        hEditCustom = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDIT, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, xEdit, DPIScale(hWnd, 170), wEdit, DPIScale(hWnd, 25), hWnd, NULL, hInst, NULL);
        hBtnEnum = CreateWindowW(WC_BUTTON, GetStringRes(IDS_BUTTON_ENUM).c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, xEnum, DPIScale(hWnd, 170), wEnum, DPIScale(hWnd, 25), hWnd, (HMENU)ID_BTN_ENUM, hInst, NULL);
        hBtnCustomApply = CreateWindowW(WC_BUTTON, GetStringRes(IDS_BUTTON_APPLY).c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, xApply, DPIScale(hWnd, 170), wApply, DPIScale(hWnd, 25), hWnd, (HMENU)ID_BTN_APPLY, hInst, NULL);

        // --- 1. 主动防御 (使用自适应宽度) ---
        int wDefense = GetScaledTextWidth(hWnd, g_hFontMain, GetStringRes(IDS_LBL_ACTIVE_DEFENCE));
        hChkDefense = CreateDarkCheckbox(hWnd, GetStringRes(IDS_LBL_ACTIVE_DEFENCE).c_str(), DPIScale(hWnd, 10), DPIScale(hWnd, 210), wDefense, DPIScale(hWnd, 25), ID_CHK_DEFENSE);
        SendMessage(hChkDefense, BM_SETCHECK, BST_CHECKED, 0);

        // --- 新增：开机自动运行 (使用自适应宽度) ---
        int wStartup = GetScaledTextWidth(hWnd, g_hFontMain, GetStringRes(IDS_LBL_AUTO_START));
        hChkStartup = CreateDarkCheckbox(hWnd, GetStringRes(IDS_LBL_AUTO_START).c_str(), DPIScale(hWnd, 10), DPIScale(hWnd, 240), wStartup, DPIScale(hWnd, 25), ID_CHK_STARTUP);
        SendMessage(hChkStartup, BM_SETCHECK, SyncAndCheckAutoStart(hWnd) ? BST_CHECKED : BST_UNCHECKED, 0);

        // --- 2. 气泡通知 (使用自适应宽度) ---
        int wNotify = GetScaledTextWidth(hWnd, g_hFontMain, GetStringRes(IDS_LBL_TRAY_BALLOON));
        hChkNotify = CreateDarkCheckbox(hWnd, GetStringRes(IDS_LBL_TRAY_BALLOON).c_str(), DPIScale(hWnd, 10), DPIScale(hWnd, 270), wNotify, DPIScale(hWnd, 25), ID_CHK_NOTIFY);
        SendMessage(hChkNotify, BM_SETCHECK, BST_CHECKED, 0);

        HDC hdc = GetDC(hWnd);
        SelectObject(hdc, g_hFontMain);
        SIZE sz;

        int curX = DPIScale(hWnd, 10);
        int curY = DPIScale(hWnd, 300);

        std::wstring tpl = GetStringRes(IDS_LBL_LOCK_NOTIF).c_str();
        std::wstring curStr = L"";
        bool isFirst = true;

        for (size_t i = 0; i < tpl.length(); ) {
            if (tpl.substr(i, 6) == L"{Time}") {
                if (!curStr.empty()) {
                    GetTextExtentPoint32W(hdc, curStr.c_str(), curStr.length(), &sz);
                    if (isFirst) {
                        // 模板的第一段变成 Checkbox！宽度 = 文本宽 + 方块宽
                        int w = sz.cx + DPIScale(hWnd, 25);
                        hChkAlert = CreateDarkCheckbox(hWnd, curStr, curX, curY, w, DPIScale(hWnd, 25), ID_CHK_ALERT);
                        SendMessage(hChkAlert, BM_SETCHECK, BST_CHECKED, 0);
                        curX += w + DPIScale(hWnd, 2);
                        isFirst = false;
                    } else {
                        CreateWindowW(WC_STATIC, curStr.c_str(), WS_CHILD | WS_VISIBLE, curX, curY + DPIScale(hWnd, 3), sz.cx, DPIScale(hWnd, 20), hWnd, NULL, hInst, NULL);
                        curX += sz.cx + DPIScale(hWnd, 2);
                    }
                    curStr = L"";
                }
                std::wstring strTime = std::to_wstring(g_AlertTimeMs.load());
                hEditTime = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDIT, strTime.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_CENTER, curX, curY, DPIScale(hWnd, 50), DPIScale(hWnd, 25), hWnd, (HMENU)ID_EDIT_TIME, hInst, NULL);
                curX += DPIScale(hWnd, 52);
                i += 6;
            }
            else if (tpl.substr(i, 7) == L"{Count}") {
                if (!curStr.empty()) {
                    GetTextExtentPoint32W(hdc, curStr.c_str(), curStr.length(), &sz);
                    if (isFirst) {
                        int w = sz.cx + DPIScale(hWnd, 25);
                        hChkAlert = CreateDarkCheckbox(hWnd, curStr, curX, curY, w, DPIScale(hWnd, 25), ID_CHK_ALERT);
                        SendMessage(hChkAlert, BM_SETCHECK, BST_CHECKED, 0);
                        curX += w + DPIScale(hWnd, 2);
                        isFirst = false;
                    } else {
                        CreateWindowW(WC_STATIC, curStr.c_str(), WS_CHILD | WS_VISIBLE, curX, curY + DPIScale(hWnd, 3), sz.cx, DPIScale(hWnd, 20), hWnd, NULL, hInst, NULL);
                        curX += sz.cx + DPIScale(hWnd, 2);
                    }
                    curStr = L"";
                }
                std::wstring strCount = std::to_wstring(g_AlertCount.load());
                hEditCount = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDIT, strCount.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_CENTER, curX, curY, DPIScale(hWnd, 30), DPIScale(hWnd, 25), hWnd, (HMENU)ID_EDIT_COUNT, hInst, NULL);
                curX += DPIScale(hWnd, 32);
                i += 7;
            }
            else {
                curStr += tpl[i];
                i++;
            }
        }
        if (!curStr.empty()) {
            GetTextExtentPoint32W(hdc, curStr.c_str(), curStr.length(), &sz);
            CreateWindowW(WC_STATIC, curStr.c_str(), WS_CHILD | WS_VISIBLE, curX, curY + DPIScale(hWnd, 3), sz.cx, DPIScale(hWnd, 20), hWnd, NULL, hInst, NULL);
        }
        ReleaseDC(hWnd, hdc);
        // ------------------------------------

        hLogEdit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDIT, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, DPIScale(hWnd, 10), DPIScale(hWnd, 330), DPIScale(hWnd, 660), DPIScale(hWnd, 240), hWnd, NULL, hInst, NULL);
        // 初始化菜单专属的追踪型 Tooltip
        g_hMenuToolTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hWnd, NULL, hInst, NULL);
        g_tiMenu.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        g_tiMenu.hwnd = hWnd;
        g_tiMenu.uId = 999; // 随意给个独立 ID 避免冲突
        g_tiMenu.lpszText = (LPWSTR)L"";
        SendMessage(g_hMenuToolTip, TTM_ADDTOOL, 0, (LPARAM)&g_tiMenu);
        UpdateWindowFont(hWnd, g_hFontMain); // 👇 使用主窗口专属字体句柄
        ApplyTheme();

        g_nid.cbSize = sizeof(NOTIFYICONDATAW); g_nid.hWnd = hWnd; g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; g_nid.uCallbackMessage = WM_TRAYICON; g_nid.hIcon = LoadIcon(NULL, IDI_SHIELD);
        wcscpy_s(g_nid.szTip, GetStringRes(IDS_TRAYICON_TIP_DEFAULT).c_str()); Shell_NotifyIconW(NIM_ADD, &g_nid);

        // 👇 2. 核心修复：必须在 ADD 之后，立刻马上显式设置 V4 版本！不能有任何操作插队。
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);

        // 👇 3. 升级完版本后，再进行图标状态刷新
        UpdateTrayIcon();
        // 👇 插入这里：如果接管了旧实例，立刻发送气泡通知

        if (g_bKilledOldInstance) {
            ShowTrayNotification(GetStringRes(IDS_NOTIF_MUTEX_TITLE).c_str(), GetStringRes(IDS_NOTIF_MUTEX_DETAIL).c_str(), NIIF_INFO);
        }

        // --- 读取注册表并自动复盘 ---
        std::wstring savedPath = LoadDevicePathFromRegistry();
        if (!savedPath.empty()) {
            ScannedDevice sdev; sdev.path = savedPath; sdev.state = STATE_VALID;
            g_ScannedDevices.push_back(sdev);

            LVITEM lvi = { 0 }; lvi.mask = LVIF_TEXT; lvi.iItem = 0; lvi.pszText = (LPWSTR)L"";
            ListView_InsertItem(hListDevices, &lvi);
            ListView_SetItemText(hListDevices, 0, 1, (LPWSTR)savedPath.c_str()); // 路径写到第 1 列
            AutoSizeListViewColumns(hListDevices); // 触发自适应

            SetWindowTextW(hEditCustom, savedPath.c_str());
            SwitchDevice(savedPath);
        }
        else {
            LogMessage(GetStringRes(IDS_LOG_INTFACE_ENUM_TIP).c_str());
        }
        g_bIsInitializing = false;
        break;
    }
    case WM_SETTINGCHANGE: if (lParam && wcscmp((LPCWSTR)lParam, L"ImmersiveColorSet") == 0) ApplyTheme(); break;
    case WM_ERASEBKGND: { HDC hdc = (HDC)wParam; RECT rc; GetClientRect(hWnd, &rc); FillRect(hdc, &rc, g_hBgBrush); return 1; }
    case WM_CTLCOLOREDIT: { HDC hdc = (HDC)wParam; SetBkMode(hdc, OPAQUE); SetTextColor(hdc, g_TextColor); SetBkColor(hdc, g_IsDarkMode ? RGB(45, 45, 45) : RGB(255, 255, 255)); return (INT_PTR)g_hEditBgBrush; }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam; HWND hCtrl = (HWND)lParam; SetTextColor(hdc, g_TextColor);
        if (hCtrl == hLogEdit) { SetBkMode(hdc, OPAQUE); SetBkColor(hdc, g_IsDarkMode ? RGB(45, 45, 45) : RGB(255, 255, 255)); return (INT_PTR)g_hEditBgBrush; }
        SetBkMode(hdc, TRANSPARENT); return (INT_PTR)g_hBgBrush;
    }

    case WM_USER_ENUM_DONE: {
        auto pDevs = (std::vector<ScannedDevice>*)wParam;
        g_ScannedDevices = std::move(*pDevs); delete pDevs;

        ListView_DeleteAllItems(hListDevices);
        for (size_t i = 0; i < g_ScannedDevices.size(); i++) {
            // 👇 替换这里的填入逻辑：
            LVITEM lvi = { 0 }; lvi.mask = LVIF_TEXT; lvi.iItem = (int)i; lvi.pszText = (LPWSTR)L"";
            ListView_InsertItem(hListDevices, &lvi);
            ListView_SetItemText(hListDevices, i, 1, (LPWSTR)g_ScannedDevices[i].path.c_str()); // 路径写到第 1 列
            // 👆 替换结束
        }
        AutoSizeListViewColumns(hListDevices); // 触发自适应
        if (g_ScannedDevices.empty()) LogMessage(GetStringRes(IDS_LOG_INTFACE_ENUM_FAILED).c_str());
        else LogMessage(GetStringRes(IDS_LOG_INTFACE_ENUM_P1).c_str() + std::to_wstring(g_ScannedDevices.size()) + GetStringRes(IDS_LOG_INTFACE_ENUM_P2).c_str());
        EnableWindow(hBtnEnum, TRUE);
        break;
    }
    case WM_ENTERIDLE: {
        // MSGF_MENU 代表当前是菜单正在空闲等待输入
        if (wParam == MSGF_MENU && g_hMenuToolTip) {
            HWND hMenuWnd = (HWND)lParam; // 强转获取当前系统菜单的高层弹窗句柄 (#32768)
            POINT pt;
            GetCursorPos(&pt);
            RECT rc;
            GetWindowRect(hMenuWnd, &rc);

            // 如果鼠标已经不在菜单的矩形范围内，立刻强制消隐 Tooltip
            if (!PtInRect(&rc, pt)) {
                SendMessage(g_hMenuToolTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&g_tiMenu);
            }
        }
        break;
    }
    case WM_MENUSELECT: {
        WORD uItem = LOWORD(wParam);
        WORD fuFlags = HIWORD(wParam);
        HMENU hMenu = (HMENU)lParam; // 获取当前弹出的菜单句柄

        // 如果菜单被关闭（移出区域），隐藏 Tooltip
        if (fuFlags == 0xFFFF && hMenu == NULL) {
            SendMessage(g_hMenuToolTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&g_tiMenu);
            break;
        }

        // 如果悬停在有效的选项上（不是分隔符，也不是不可点击的顶层栏）
        if (!(fuFlags & (MF_POPUP | MF_SEPARATOR))) {
            std::wstring tip = L"";
            if (uItem == ID_TRAY_TOGGLE_LOCK) tip = GetStringRes(IDS_TRAY_TOGGLE_LOCK).c_str();
            else if (uItem == ID_TRAY_DEFENSE) tip = GetStringRes(IDS_TRAY_DEFENSE).c_str();
            else if (uItem == ID_TRAY_RESTORE) tip = GetStringRes(IDS_TRAY_RESTORE).c_str(); //L"打开图形控制面板，查看底层拦截日志"
            else if (uItem == ID_TRAY_SETTINGS) tip = GetStringRes(IDS_TRAY_SETTINGS).c_str(); //L"查看作者信息与项目设置";
            else if (uItem == ID_TRAY_EXIT) tip = GetStringRes(IDS_TRAY_EXIT).c_str(); // L"彻底退出程序，释放硬件占用通道";

            if (!tip.empty()) {
                g_tiMenu.lpszText = (LPWSTR)tip.c_str();
                SendMessage(g_hMenuToolTip, TTM_UPDATETIPTEXT, 0, (LPARAM)&g_tiMenu);

                // 👇 核心修复：直接向 GDI 借字体，同步精准算出文字渲染宽度，拒绝滞后！
                SIZE sz;
                HDC hdcTT = GetDC(g_hMenuToolTip);
                HFONT hFont = (HFONT)SendMessage(g_hMenuToolTip, WM_GETFONT, 0, 0);
                HGDIOBJ hOld = SelectObject(hdcTT, hFont);
                GetTextExtentPoint32W(hdcTT, tip.c_str(), tip.length(), &sz);
                SelectObject(hdcTT, hOld);
                ReleaseDC(g_hMenuToolTip, hdcTT);

                // 真实的物理宽度 = 文字宽度 + Tooltip 两侧自带的 Padding (约12px)
                int tipWidth = sz.cx + DPIScale(hWnd, 12);

                int itemIndex = -1;
                for (int i = 0; i < GetMenuItemCount(hMenu); i++) {
                    if (GetMenuItemID(hMenu, i) == uItem) { itemIndex = i; break; }
                }

                if (itemIndex != -1) {
                    RECT rcItem;
                    if (GetMenuItemRect(NULL, hMenu, itemIndex, &rcItem)) {
                        int screenW = GetSystemMetrics(SM_CXSCREEN);
                        int gap = DPIScale(hWnd, 5);

                        int tipX = rcItem.right + gap;
                        // 完美对齐：左边缘 - 精准宽度 - 间距
                        if (tipX + tipWidth > screenW) {
                            tipX = rcItem.left - tipWidth - gap;
                        }
                        SendMessage(g_hMenuToolTip, TTM_TRACKPOSITION, 0, MAKELONG(tipX, rcItem.top));
                    }
                }
                SendMessage(g_hMenuToolTip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&g_tiMenu);
            } else {
                SendMessage(g_hMenuToolTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&g_tiMenu);
            }
        }
        else {
            SendMessage(g_hMenuToolTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&g_tiMenu);
        }
        break;
    }
    case WM_COMMAND: {
        if (HIWORD(wParam) == EN_CHANGE) {
            if (g_bIsInitializing) break;
            if (LOWORD(wParam) == ID_EDIT_TIME) {
                g_AlertTimeMs = GetDlgItemInt(hWnd, ID_EDIT_TIME, NULL, FALSE);
                SaveAlertConfigToRegistry(); // 👇 新增：数值改变时立刻持久化
            }
            if (LOWORD(wParam) == ID_EDIT_COUNT) {
                g_AlertCount = GetDlgItemInt(hWnd, ID_EDIT_COUNT, NULL, FALSE);
                SaveAlertConfigToRegistry(); // 👇 新增：数值改变时立刻持久化
            }
        }
        else if (LOWORD(wParam) == ID_BTN_ENUM) {
            EnableWindow(hBtnEnum, FALSE);
            LogMessage(GetStringRes(IDS_LOG_INTFACE_ENUMING).c_str());
            std::thread(AsyncEnumerateThread).detach();
        }
        else if (LOWORD(wParam) == ID_BTN_APPLY) {
            WCHAR buf[512]; GetWindowTextW(hEditCustom, buf, 512); SwitchDevice(buf);
            SaveDevicePathToRegistry(buf);
        }
        else if (LOWORD(wParam) == ID_CHK_DEFENSE) {
            g_ActiveDefense = (SendMessage(hChkDefense, BM_GETCHECK, 0, 0) == BST_CHECKED);
            // 👇 新增：如果开启了防御，且当前键盘是锁定的，立刻强行解锁
            if (g_ActiveDefense && g_IsLocked) {
                HardwareTest(g_CurrentDevicePath, false);
                g_IsLocked = false;
            }
            UpdateTrayIcon();
        }
        else if (LOWORD(wParam) == ID_CHK_NOTIFY) {
            g_EnableNotify = (SendMessage(hChkNotify, BM_GETCHECK, 0, 0) == BST_CHECKED);
        }
        // (放在 else if (LOWORD(wParam) == ID_CHK_NOTIFY)... 下方)
        else if (LOWORD(wParam) == ID_CHK_ALERT) g_EnableAlert = (SendMessage(hChkAlert, BM_GETCHECK, 0, 0) == BST_CHECKED);

        // 托盘菜单处理
        else if (LOWORD(wParam) == ID_TRAY_TOGGLE_LOCK) {
            bool targetLockState = !g_IsLocked;
            if (targetLockState && g_ActiveDefense) {
                g_ActiveDefense = false; SendMessage(hChkDefense, BM_SETCHECK, BST_UNCHECKED, 0);
                UpdateTrayIcon(); // 加上这行
            }
            if (HardwareTest(g_CurrentDevicePath, targetLockState)) g_IsLocked = targetLockState;
        }
        else if (LOWORD(wParam) == ID_TRAY_DEFENSE) {
            g_ActiveDefense = !g_ActiveDefense;
            SendMessage(hChkDefense, BM_SETCHECK, g_ActiveDefense ? BST_CHECKED : BST_UNCHECKED, 0);
            // 👇 新增：如果开启了防御，且当前键盘是锁定的，立刻强行解锁
            if (g_ActiveDefense && g_IsLocked) {
                HardwareTest(g_CurrentDevicePath, false);
                g_IsLocked = false;
            }
            UpdateTrayIcon(); // 加上这行
        }
        else if (LOWORD(wParam) == ID_TRAY_RESTORE) {
            if (IsIconic(hWnd)) {
                // 如果是最小化，直接走系统原生恢复
                ShowWindow(hWnd, SW_RESTORE);
            }
            else if (!IsWindowVisible(hWnd)) {
                // 如果是隐藏状态，走混合策略
                if (g_bIsFirstShowMain) {
                    ShowWindow(hWnd, SW_RESTORE);
                    g_bIsFirstShowMain = false;
                }
                else {
                    SmoothFadeInWindow(hWnd);
                }
            }
            else {
                ShowWindow(hWnd, SW_RESTORE);
            }
            SetForegroundWindow(hWnd);
        }
        else if (LOWORD(wParam) == ID_TRAY_EXIT) DestroyWindow(hWnd);
        else if (LOWORD(wParam) == ID_TRAY_SETTINGS) {
            if (!g_hSettingsWnd) {
                WNDCLASSW wc = { 0 }; wc.lpfnWndProc = SettingsWndProc; wc.hInstance = hInst; wc.lpszClassName = L"SettingsWndClass"; RegisterClassW(&wc);

                int setW = MulDiv(450, GetDpiForWindow(hWnd), 96);
                int setH = MulDiv(180, GetDpiForWindow(hWnd), 96);

                // 让设置窗口在主窗口的中心 Spawn
                RECT rcMain; GetWindowRect(hWnd, &rcMain);
                int setX = rcMain.left + (rcMain.right - rcMain.left - setW) / 2;
                int setY = rcMain.top + (rcMain.bottom - rcMain.top - setH) / 2;

                // 确保即使主窗口被拖到了屏幕边缘，设置窗口生成时也不会越出屏幕
                HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(MONITORINFO) };
                if (GetMonitorInfoW(hMon, &mi)) {
                    if (setX + setW > mi.rcWork.right) setX = mi.rcWork.right - setW;
                    if (setY + setH > mi.rcWork.bottom) setY = mi.rcWork.bottom - setH;
                    if (setX < mi.rcWork.left) setX = mi.rcWork.left;
                    if (setY < mi.rcWork.top) setY = mi.rcWork.top; // 标题栏必须可见
                }

                // 注入计算好的坐标
                g_hSettingsWnd = CreateWindowW(L"SettingsWndClass", GetStringRes(IDS_WNDNAME_ABOUT).c_str(),
                                               WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_MINIMIZEBOX & ~WS_THICKFRAME,
                                               setX, setY, setW, setH,
                                               hWnd, NULL, hInst, NULL);
            }
            // 👇 核心修改：用淡入动画替代瞬间显示
            if (!IsWindowVisible(g_hSettingsWnd)) {
                // 👇 设置窗口也享受同等待遇
                if (g_bIsFirstShowSettings) {
                    ShowWindow(g_hSettingsWnd, SW_SHOW);
                    g_bIsFirstShowSettings = false;
                }
                else {
                    SmoothFadeInWindow(g_hSettingsWnd);
                }
            }
            //SetForegroundWindow(g_hSettingsWnd);
        }
        else if (LOWORD(wParam) == ID_CHK_STARTUP) {
            bool isChecked = (SendMessage(hChkStartup, BM_GETCHECK, 0, 0) == BST_CHECKED);
            SetAutoStart(isChecked); // 用户点击后，调用封装好的函数写注册表
        }
        break;
    }
    case WM_CONTEXTMENU: {
        HWND hCtrl = (HWND)wParam;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (hCtrl == hListDevices) {
            if (pt.x == -1 && pt.y == -1) {
                int sel = ListView_GetNextItem(hCtrl, -1, LVNI_SELECTED);
                if (sel != -1) {
                    RECT rc; ListView_GetItemRect(hCtrl, sel, &rc, LVIR_BOUNDS);
                    pt.x = rc.left + 10; pt.y = rc.top + 10; ClientToScreen(hCtrl, &pt);
                }
            }
            POINT ptClient = pt; ScreenToClient(hCtrl, &ptClient);
            LVHITTESTINFO lvhti = { 0 }; lvhti.pt = ptClient;
            ListView_SubItemHitTest(hCtrl, &lvhti);
            int selRow = lvhti.iItem;
            if (selRow != -1) {
                HMENU hPopup = CreatePopupMenu();
                AppendMenuW(hPopup, MF_STRING, 10001, GetStringRes(IDS_LISTBOX_MENU_COPY).c_str());
                if (TrackPopupMenu(hPopup, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, NULL) == 10001) {
                    WCHAR text[1024] = { 0 };
                    ListView_GetItemText(hCtrl, selRow, 1, text, 1024); // 提取第 1 列的路径
                    if (wcslen(text) > 0 && OpenClipboard(hWnd)) {
                        EmptyClipboard();
                        HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, (wcslen(text) + 1) * static_cast<DWORD>(sizeof(WCHAR)));
                        if (hGlob) {
                            memcpy(GlobalLock(hGlob), text, (wcslen(text) + 1) * static_cast<DWORD>(sizeof(WCHAR)));
                            GlobalUnlock(hGlob); SetClipboardData(CF_UNICODETEXT, hGlob);
                        }
                        CloseClipboard();
                    }
                }
                DestroyMenu(hPopup);
            }
        }
        break;
    }

                   // --- 高级交互自绘区 ---
    case WM_NOTIFY: {
        LPNMHDR pnmh = (LPNMHDR)lParam;
        if (pnmh->hwndFrom == hListDevices) {
            if (pnmh->code == NM_CUSTOMDRAW) {
                LPNMLVCUSTOMDRAW lplvcd = (LPNMLVCUSTOMDRAW)lParam;
                if (lplvcd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (lplvcd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;

                if (lplvcd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                    if (lplvcd->iSubItem == 0) { // 强接管交互列的绘制
                        int idx = (int)lplvcd->nmcd.dwItemSpec;
                        if (idx >= 0 && idx < g_ScannedDevices.size()) {
                            TestState state = g_ScannedDevices[idx].state;
                            // 👇 替换 RECT rc 获取方式：利用当前绘制上下文并强制限制宽度为第0列的宽度
                            RECT rc = lplvcd->nmcd.rc;
                            rc.right = rc.left + ListView_GetColumnWidth(hListDevices, 0);
                            // 👆 替换结束
                            HDC hdc = lplvcd->nmcd.hdc; SetBkMode(hdc, TRANSPARENT);

                            if (state == STATE_UNTESTED) {
                                SetTextColor(hdc, RGB(100, 200, 255)); DrawTextW(hdc, GetStringRes(IDS_LISTBOX_CELL_TEST).c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                            }
                            else if (state == STATE_TESTING) {
                                int mid = rc.left + (rc.right - rc.left) / 2;
                                RECT rcValid = { rc.left, rc.top, mid, rc.bottom }; RECT rcInvalid = { mid, rc.top, rc.right, rc.bottom };
                                SetTextColor(hdc, RGB(255, 80, 80)); DrawTextW(hdc, GetStringRes(IDS_LISTBOX_CELL_WORKS).c_str(), -1, &rcValid, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                                SetTextColor(hdc, RGB(150, 150, 150)); DrawTextW(hdc, GetStringRes(IDS_LISTBOX_CELL_NOTWORK).c_str(), -1, &rcInvalid, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                            }
                            else if (state == STATE_INVALID) {
                                SetTextColor(hdc, RGB(150, 150, 150)); DrawTextW(hdc, GetStringRes(IDS_LISTBOX_CELL_NOTWORK).c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                            }
                            else if (state == STATE_VALID) {
                                SetTextColor(hdc, RGB(255, 80, 80)); DrawTextW(hdc, GetStringRes(IDS_LISTBOX_CELL_WORKS).c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                            }
                            return CDRF_SKIPDEFAULT; // 跳过原版文本绘制，直接用我们的手绘内容
                        }
                    }
                    lplvcd->clrText = g_TextColor; return CDRF_NEWFONT;
                }
            }
            // 鼠标点击分发逻辑
            if (pnmh->code == NM_CLICK) {
                LPNMITEMACTIVATE pnmia = (LPNMITEMACTIVATE)lParam;
                if (pnmia->iItem >= 0 && pnmia->iItem < g_ScannedDevices.size() && pnmia->iSubItem == 0) {
                    int idx = pnmia->iItem;
                    if (g_ScannedDevices[idx].state == STATE_UNTESTED) {
                        StartInteractiveTest(idx);
                    }
                    else if (g_ScannedDevices[idx].state == STATE_TESTING) {
                        // 👇 替换中点计算方式：直接取第0列宽度的一半，完美避开 Win32 底层坑
                        int mid = ListView_GetColumnWidth(hListDevices, 0) / 2;
                        if (pnmia->ptAction.x < mid) EndInteractiveTest(idx, true);
                        else EndInteractiveTest(idx, false);
                        // 👆 替换结束
                    }
                }
            }
        }
        break;
    }

    case WM_USER_LOG: {
        BSTR bstrMsg = (BSTR)lParam;
        if (bstrMsg) { int len = GetWindowTextLength(hLogEdit); SendMessage(hLogEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len); SendMessage(hLogEdit, EM_REPLACESEL, 0, (LPARAM)bstrMsg); SysFreeString(bstrMsg); }
        break;
    }
    case WM_USER_NOTIFY:
        UpdateTrayIcon(); // 👇 新增：无论收到什么通知（包括空通知），都先刷一下图标状态
        if (wParam == NOTIFY_SUCCESS) ShowTrayNotification(GetStringRes(IDS_NOTIF_DEFEND_SUCCESS_TITLE).c_str(), GetStringRes(IDS_NOTIF_DEFEND_SUCCESS_DETAIL).c_str(), NIIF_INFO);
        else if (wParam == NOTIFY_WARNING) ShowTrayNotification(GetStringRes(IDS_NOTIF_LOCKED_TITLE).c_str(), GetStringRes(IDS_NOTIF_LOCKED_DETAIL).c_str(), NIIF_WARNING);
        else if (wParam == NOTIFY_ERROR) ShowTrayNotification(GetStringRes(IDS_NOTIF_FAILED_TITLE).c_str(), GetStringRes(IDS_NOTIF_FAILED_DETAIL).c_str(), NIIF_ERROR);
        break;
    case WM_ACTIVATE: {
        // 当窗口失去激活状态（比如点击了其他程序的托盘、其他窗口或任务栏）
        if (LOWORD(wParam) == WA_INACTIVE) {
            // 强制发送取消模式信号，瞬间终结 TrackPopupMenu 的死循环
            SendMessage(hWnd, WM_CANCELMODE, 0, 0);
        }
        break; // 记得 break，让系统继续走默认的 DefWindowProc
    }
    case WM_TRAYICON: {
        if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            if (IsIconic(hWnd)) {
                // 👇 状态 1：如果是最小化状态，直接调用系统恢复，白嫖完美的系统原生任务栏展开动画
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            }
            else if (IsWindowVisible(hWnd)) {
                // 👇 状态 2：只有当窗口“可见”且“没有最小化”时，双击才是隐藏
                ShowWindow(hWnd, SW_HIDE);
            }
            else {
                // 👇 状态 3：完全隐藏状态下的唤醒逻辑（走我们的高帧率淡入）
                if (g_bIsFirstShowMain) {
                    ShowWindow(hWnd, SW_RESTORE);
                    g_bIsFirstShowMain = false;
                }
                else {
                    SmoothFadeInWindow(hWnd);
                }
                SetForegroundWindow(hWnd);
            }
        }
        else if (LOWORD(lParam) == WM_CONTEXTMENU) {
            POINT pt; GetCursorPos(&pt);

            // 👇 核心修复 1：焦点三连击。强行榨干系统权限，确保我们的隐藏窗口拿到最高优先级
            SetForegroundWindow(hWnd);
            SetActiveWindow(hWnd);
            SetFocus(hWnd);

            HMENU hMenu = CreatePopupMenu();
            std::wstring stateText = GetStringRes(IDS_TRAYMENU_STATUS).c_str(); stateText += g_ActiveDefense ? GetStringRes(IDS_TRAYMENU_STATUS_DEFENCE_ACTIVE).c_str() : GetStringRes(IDS_TRAYMENU_STATUS_PAUSED).c_str(); stateText += g_IsLocked ? GetStringRes(IDS_TRAYMENU_STATUS_KEY_LOCKED).c_str() : GetStringRes(IDS_TRAYMENU_STATUS_KEY_IDLE).c_str();
            AppendMenuW(hMenu, MF_STRING | MF_DISABLED, ID_TRAY_STATE, stateText.c_str()); AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE_LOCK, g_IsLocked ? GetStringRes(IDS_TRAYMENU_MANUAL_UNLOCK).c_str() : GetStringRes(IDS_TRAYMENU_MANUAL_LOCK).c_str());
            AppendMenuW(hMenu, MF_STRING | (g_ActiveDefense ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_DEFENSE, GetStringRes(IDS_TRAYMENU_TOGGLE_ACTIVE_DEFENCE).c_str());
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_RESTORE, GetStringRes(IDS_TRAYMENU_OPEN_CTRL_PNL).c_str()); AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, GetStringRes(IDS_WNDNAME_ABOUT).c_str());
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL); AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, GetStringRes(IDS_TRAYMENU_EXIT).c_str());

            // 👇 核心修复 2：加入 TPM_RETURNCMD | TPM_NONOTIFY。
            // 这会让 TrackPopupMenu 变成同步阻塞模式，不再依赖不可靠的后台消息队列，点击外面必定瞬间销毁
            // int cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, NULL);
            // 强制菜单的右下角对齐鼠标，实现向左、向上弹出，完美避开通知遮挡
            int cmd = TrackPopupMenu(hMenu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);

            SendMessage(g_hMenuToolTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&g_tiMenu);
            PostMessage(hWnd, WM_NULL, 0, 0);

            // 👇 核心修复 3：因为上面变成了同步模式，我们需要在这里手动将命令派发给现有的 WM_COMMAND
            if (cmd != 0) {
                SendMessage(hWnd, WM_COMMAND, cmd, 0);
            }
        }
        break;
    }
    case WM_DPICHANGED: {
        // 从参数中解出 Windows 建议的主窗口推荐尺寸与推荐屏幕坐标
        int newDpi = LOWORD(wParam);
        RECT* prcNewWindow = (RECT*)lParam;

        // 1. 首先平滑缩放并搬移外层主窗体物理框架
        SetWindowPos(hWnd,
                     NULL,
                     prcNewWindow->left,
                     prcNewWindow->top,
                     prcNewWindow->right - prcNewWindow->left,
                     prcNewWindow->bottom - prcNewWindow->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);

        // 2. 重新构建适配目标高分屏的最佳无损 Microsoft YaHei UI 字体句柄
        UpdateWindowFont(hWnd, g_hFontMain);

        // 3. 呼叫排版布局引擎，让内部的所有控件根据新 DPI 毫米级重新对齐
        RearrangeControlsLayout(hWnd, newDpi);

        // 4. 重刷自适应列表列宽并强行引发主题环境重绘制
        AutoSizeListViewColumns(hListDevices);
        ApplyTheme();
        break;
    }
    case WM_CLOSE: ShowWindow(hWnd, SW_HIDE); return 0;
    case WM_DESTROY:
        g_Running = false; if (g_hDevice != INVALID_HANDLE_VALUE) CancelIoEx(g_hDevice, NULL);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_hBgBrush) DeleteObject(g_hBgBrush); if (g_hEditBgBrush) DeleteObject(g_hEditBgBrush);
        PostQuitMessage(0); break;
    default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}
// 新增：根据窗口尺寸，计算出贴靠右下角且绝对安全的生成坐标
void GetSafeBottomRightPosition(int width, int height, int& outX, int& outY) {
    // 1. 获取当前鼠标所在的显示器 (多屏适配)
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);

    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (GetMonitorInfoW(hMonitor, &mi)) {
        // 使用系统 DPI 计算一个 80px 的安全边距，避免窗口死死贴住屏幕边缘
        int gap = MulDiv(80, GetDpiForSystem(), 96);

        // 2. 默认定位在工作区（已扣除任务栏）的右下角
        outX = mi.rcWork.right - width - gap;
        outY = mi.rcWork.bottom - height - gap;

        // 3. 极端分辨率/超大字体下的安全逃生舱口 (Safety Bounds Check)
        // 规则 A：如果窗口比屏幕还宽，或者左侧越界，强制靠左
        if (outX < mi.rcWork.left) {
            outX = mi.rcWork.left;
        }
        // 规则 B (最高优先级)：如果窗口比屏幕还高，或者顶部越界，强制靠顶！
        // 这绝对保证了窗口的【标题栏】一定在屏幕工作区内，用户随时可以按住它拖走。
        if (outY < mi.rcWork.top) {
            outY = mi.rcWork.top;
        }
    } else {
        outX = CW_USEDEFAULT;
        outY = CW_USEDEFAULT;
    }
}
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {

#ifdef _DEBUG
    // ==========================================
    // 调试模式专用：全局强制接管整个进程的 UI 语言
    // ==========================================

    ULONG numLanguages = 0;

    // 测试美国英文 (en-US)
    // 注意：字符串必须是双 \0 结尾
    PCZZWSTR langUS = L"en-US\0";
    SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, langUS, &numLanguages);

    // // 测试繁体中文 (zh-TW)
    // PCZZWSTR langTW = L"zh-TW\0";
    // SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, langTW, &numLanguages);

#endif

    // ==========================================
    // 自动判断并 Fallback 未知语言到英文
    // ==========================================
    LANGID sysLang = GetUserDefaultUILanguage();
    // 如果系统主语言既不是中文(LANG_CHINESE) 也不是英文(LANG_ENGLISH)，强制将本程序降级为英文
    if (PRIMARYLANGID(sysLang) != LANG_CHINESE && PRIMARYLANGID(sysLang) != LANG_ENGLISH) {
        ULONG numLanguages = 0;
        PCZZWSTR langUS = L"en-US\0"; // 注意双 \0 结尾
        SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, langUS, &numLanguages);
    }

    // 新增：强制设置 AppUserModelID，试图覆盖通知中心的 .exe 名称
    SetCurrentProcessExplicitAppUserModelID(L"WinKey.Defender.App");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    EnableGlobalAppDarkMode(); hInst = hInstance;
    HWND hOldWnd = FindWindowW(L"WinKeyDefenderClass", NULL);
    if (hOldWnd) {
        // 发送我们定义的退出命令，旧实例会执行 DestroyWindow 和 Shell_NotifyIconW(NIM_DELETE)
        SendMessageW(hOldWnd, WM_COMMAND, ID_TRAY_EXIT, 0);
        Sleep(200); // 稍微等待旧实例完成资源释放
        g_bKilledOldInstance = true; // 👇 标记：我们刚刚结束了一个旧实例
    }
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, WndProc, 0, 0, hInstance, NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, L"WinKeyDefenderClass", NULL };
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1)); // 任务栏和 Alt+Tab 大图标
    wcex.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1)); // 窗口左上角小图标
    RegisterClassExW(&wcex);

    // --- 替换原有的主窗口创建代码 ---
    int mainW = MulDiv(700, GetDpiForSystem(), 96);
    int mainH = MulDiv(620, GetDpiForSystem(), 96);

    int mainX, mainY;
    GetSafeBottomRightPosition(mainW, mainH, mainX, mainY);

    g_hMainWnd = CreateWindowExW(0, L"WinKeyDefenderClass", GetStringRes(IDS_WNDNAME_CTRL_PNL).c_str(),
                                 WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                                 mainX, mainY, mainW, mainH,
                                 NULL, NULL, hInstance, NULL);
    // ---------------------------------

    if (!g_hMainWnd) return FALSE;

    g_MonitorThread = std::thread(HIDMonitorThreadWorker);
    // ShowWindow(g_hMainWnd, nCmdShow);

    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    if (g_MonitorThread.joinable()) g_MonitorThread.join();
    return (int)msg.wParam;
}