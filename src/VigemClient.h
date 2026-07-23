// SPDX-License-Identifier: GPL-3.0
// Copyright (c) 2026 p2b
#pragma once
//
//  VigemClient.h — ViGEmClient.dll 的运行时动态加载封装
//  ------------------------------------------------------------
//  程序在运行时用 LoadLibrary 加载 ViGEmClient.dll,创建一个虚拟
//  Xbox 360 手柄,把摇杆值(XUSB_REPORT)送进系统。这样编译时
//  完全不需要 ViGEm 的头文件或 .lib,只要运行时能找到 dll 即可。
//
//  前置条件:系统已安装 ViGEmBus 驱动(内核总线驱动)。
//
#include <windows.h>
#include <string>
#include <cstdio>   // 探针 printf(右摇杆漂移排查)

extern bool g_debugLog;   // 定义在 main.cpp;命令行带 -d 时为 true,保留控制台

namespace vigem {

// ---- ViGEmClient 的不透明句柄类型 ----
struct VIGEM_CLIENT_IMPL;
struct VIGEM_TARGET_IMPL;
using PVIGEM_CLIENT = VIGEM_CLIENT_IMPL*;
using PVIGEM_TARGET = VIGEM_TARGET_IMPL*;

// ---- 错误码(NTSTATUS 风格,与 ViGEmClient 1.21 的 Client.h 一致)----
enum VIGEM_ERROR : unsigned int {
    ERROR_NONE              = 0x20000000u,
    ERROR_BUS_NOT_FOUND     = 0xE0000001u,
    ERROR_NO_FREE_SLOT      = 0xE0000002u,
    ERROR_INVALID_TARGET    = 0xE0000003u,
    ERROR_REMOVAL_FAILED    = 0xE0000004u,
    ERROR_ALREADY_CONNECTED = 0xE0000005u,
    ERROR_BUS_ACCESS_FAILED = 0xE0000009u,
};

// ---- XInput / XUSB 报告结构(与 Windows xusb.h 一致,12 字节)----
struct XUSB_REPORT {
    unsigned short wButtons;
    unsigned char  bLeftTrigger;
    unsigned char  bRightTrigger;
    short          sThumbLX;
    short          sThumbLY;
    short          sThumbRX;
    short          sThumbRY;
};

// ---- 函数指针类型 ----
using fn_alloc        = PVIGEM_CLIENT (*)();
using fn_free         = void (*)       (PVIGEM_CLIENT);
using fn_connect      = VIGEM_ERROR (*) (PVIGEM_CLIENT);
using fn_disconnect   = void (*)       (PVIGEM_CLIENT);
using fn_tg_alloc     = PVIGEM_TARGET (*)();
using fn_tg_free      = void (*)       (PVIGEM_TARGET);
using fn_tg_add       = VIGEM_ERROR (*) (PVIGEM_CLIENT, PVIGEM_TARGET);
using fn_tg_remove    = VIGEM_ERROR (*) (PVIGEM_CLIENT, PVIGEM_TARGET);
using fn_tg_update    = VIGEM_ERROR (*) (PVIGEM_CLIENT, PVIGEM_TARGET, XUSB_REPORT);
using fn_tg_get_index = unsigned int (*) (PVIGEM_TARGET);

// 把 ViGEm 错误码翻译成人话
inline const char* err_str(VIGEM_ERROR e) {
    switch (e) {
        case ERROR_NONE:              return "成功";
        case ERROR_BUS_NOT_FOUND:     return "未找到 ViGEmBus 驱动(请先安装 ViGEmBus)";
        case ERROR_NO_FREE_SLOT:      return "没有空闲的手柄槽位";
        case ERROR_INVALID_TARGET:    return "无效目标";
        case ERROR_REMOVAL_FAILED:    return "移除目标失败";
        case ERROR_ALREADY_CONNECTED: return "已连接";
        case ERROR_BUS_ACCESS_FAILED: return "访问总线失败";
        default:                      return "未知错误";
    }
}

// 取 exe 所在目录拼上文件名,用于优先从 exe 目录加载 dll
inline std::wstring exe_dir_file(const wchar_t* file) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    p += file;
    return p;
}

// ---- 高层封装 ----
class Pad {
   public:
    Pad() = default;
    ~Pad() { shutdown(); }
    Pad(const Pad&) = delete;
    Pad& operator=(const Pad&) = delete;

    // 加载 dll 并建立虚拟手柄。成功返回 true。
    bool init() {
        // 1) 加载 dll:先试 exe 目录,再试系统搜索路径
        m_dll = LoadLibraryW(exe_dir_file(L"ViGEmClient.dll").c_str());
        if (!m_dll) m_dll = LoadLibraryW(L"ViGEmClient.dll");
        if (!m_dll) {
            m_errMsg = "找不到 ViGEmClient.dll";
            return false;
        }

        // 2) 解析需要的导出函数
        auto get = [&](const char* name) -> FARPROC { return GetProcAddress(m_dll, name); };
        m_alloc        = (fn_alloc)        get("vigem_alloc");
        m_free         = (fn_free)         get("vigem_free");
        m_connect      = (fn_connect)      get("vigem_connect");
        m_disconnect   = (fn_disconnect)   get("vigem_disconnect");
        m_tg_alloc     = (fn_tg_alloc)     get("vigem_target_x360_alloc");
        m_tg_free      = (fn_tg_free)      get("vigem_target_free");
        m_tg_add       = (fn_tg_add)       get("vigem_target_add");
        m_tg_remove    = (fn_tg_remove)    get("vigem_target_remove");
        m_tg_update    = (fn_tg_update)    get("vigem_target_x360_update");
        m_tg_get_index = (fn_tg_get_index) get("vigem_target_get_index");

        if (!m_alloc || !m_connect || !m_tg_alloc || !m_tg_add || !m_tg_update) {
            m_errMsg = "ViGEmClient.dll 版本不兼容(缺少导出函数)";
            return false;
        }

        // 3) 连接总线
        m_client = m_alloc();
        if (!m_client) {
            m_errMsg = "vigem_alloc 失败";
            return false;
        }
        VIGEM_ERROR e = m_connect(m_client);
        if (e == ERROR_ALREADY_CONNECTED) e = ERROR_NONE;
        if (e != ERROR_NONE) {
            m_errMsg = err_str(e);
            return false;
        }
        m_connected = true;

        // 4) 创建并添加 Xbox360 目标
        m_target = m_tg_alloc();
        if (!m_target) {
            m_errMsg = "vigem_target_x360_alloc 失败";
            return false;
        }
        e = m_tg_add(m_client, m_target);
        if (e != ERROR_NONE) {
            m_errMsg = err_str(e);
            return false;
        }
        m_added = true;
        // target add 后到 input_thread 首次 WM_TIMER(~8ms)之间没有报告,虚拟手柄会处于
        // "未初始化报告"状态;某些游戏此刻做摇杆中心校准会采到非 0 默认值。立即发一份全 0
        // 报告填补空窗,让游戏第一眼看到的就是干净的 0(右摇杆漂移/视角左转排查)。
        update_left_stick(0, 0);
        return true;
    }

    // 把左摇杆值(范围 -32768 ~ 32767)发出去
    void update_left_stick(short lx, short ly) {
        if (!m_added) return;
        XUSB_REPORT r{};          // 整体清零:wButtons/triggers/四个摇杆轴全 0
        r.sThumbLX = lx;
        r.sThumbLY = ly;
        // [诊断探针] 代码层面从不写右摇杆(RX/RY 恒为 r{} 的 0)。若此告警响起,
        // 说明某处给右摇杆喂了非 0(与"静止漂移致视角左转"排查相关)。正常运行静默。
        if (g_debugLog && (r.sThumbRX != 0 || r.sThumbRY != 0)) {
            std::printf("[WARN] 右摇杆非零报告 RX=%d RY=%d (LX=%d LY=%d)\n",
                        (int)r.sThumbRX, (int)r.sThumbRY, (int)r.sThumbLX, (int)r.sThumbLY);
        }
        m_tg_update(m_client, m_target, r);
    }

    // 立即把摇杆归零
    void release() { update_left_stick(0, 0); }

    unsigned int index() const { return m_tg_get_index ? m_tg_get_index(m_target) : 0; }
    const char*  error() const { return m_errMsg; }

   private:
    void shutdown() {
        if (m_added && m_client && m_target && m_tg_remove) m_tg_remove(m_client, m_target);
        if (m_target && m_tg_free) m_tg_free(m_target);
        if (m_connected && m_client && m_disconnect) m_disconnect(m_client);
        if (m_client && m_free) m_free(m_client);
        if (m_dll) FreeLibrary(m_dll);
        m_added = m_connected = false;
        m_target = nullptr;
        m_client = nullptr;
        m_dll = nullptr;
    }

    HMODULE        m_dll{};
    PVIGEM_CLIENT  m_client{};
    PVIGEM_TARGET  m_target{};
    bool           m_connected{};
    bool           m_added{};
    const char*    m_errMsg{};

    fn_alloc        m_alloc{};
    fn_free         m_free{};
    fn_connect      m_connect{};
    fn_disconnect   m_disconnect{};
    fn_tg_alloc     m_tg_alloc{};
    fn_tg_free      m_tg_free{};
    fn_tg_add       m_tg_add{};
    fn_tg_remove    m_tg_remove{};
    fn_tg_update    m_tg_update{};
    fn_tg_get_index m_tg_get_index{};
};

}  // namespace vigem
