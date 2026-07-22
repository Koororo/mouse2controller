// SPDX-License-Identifier: GPL-3.0
// Copyright (c) 2026 p2b
//
//  mouse2controller — 鼠标侧键 + 键盘 WASD → 虚拟手柄左摇杆(方向模式)
//  设置窗口:推力/平滑/回中/旋转 + 触发键,旋转带方向指示器,实时生效。
//
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM/GET_Y_LPARAM(WM_NCHITTEST)
#include <commctrl.h>
#include <d3d9.h>     // DirectX9(ImGui 渲染后端)
#include <mmsystem.h> // timeBeginPeriod:提高定时器分辨率(打破后台节流)
#include <conio.h>   // _kbhit / _getch
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cwchar>     // swprintf
#include <mutex>      // std::mutex/g_curveMutex:保护曲线数据(UI↔输入线程)
#include <atomic>     // std::atomic/g_gearsHovered:UI 写 hover 状态 ↔ 输入线程 wheel_gear 读

#pragma comment(lib, "winmm.lib")  // mmsystem(timeBeginPeriod/timeKillPeriod)

// 应用图标资源 ID(与 res/resource.rc 的 #define IDI_APPICON 101 一致)
#define IDI_APPICON 101

#include "config.h"
#include "VigemClient.h"
#include "StickMapper.h"
#include "CurveMap.h"
#include "GearList.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
// ImGui win32 后端不把 WndProc handler 导出到头,需手动 extern 声明
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// 运行时可调参数(初始值取自 config.h,设置窗口可改)
float g_mag     = cfg::MAGNITUDE;                       // 推力(占摇杆满偏比例);滑块/挡位范围放开为 [0,1]
float g_smooth  = cfg::SMOOTHING;
float g_return  = cfg::RETURN_SPEED;
float g_rotate  = 0.0f;  // 输出方向旋转(度,顺时针为正)

// 命令行带 -d 时为 true(保留控制台 + lag.log 落盘)。置于全局以供 VigemClient.h 的探针 extern 访问。
bool g_debugLog = false;

namespace ui { void wheel_gear(int wheel_delta); void shift_gear(int steps, bool wrap); void on_capture_done(); void on_capture_cancel(); HWND hwnd(); }  // 前向声明,供钩子/WM_INPUT 调用

namespace {

// ============================================================
//  触发键(可绑定:鼠标按键 1-5 / 键盘 vkCode)
// ============================================================
struct TriggerKey {
    enum Kind { Mouse, Key, Wheel } kind = Mouse;
    int code = 4;  // Mouse: 1=左 2=右 3=中 4=X1 5=X2;Key: vkCode;Wheel: 忽略(滚轮自带方向)
    bool operator==(const TriggerKey& o) const { return kind == o.kind && code == o.code; }
};

// button index 1-5 -> Raw Input 的 {DOWN, UP} flag 对
struct MouseFlagPair { USHORT down, up; };
inline MouseFlagPair mouse_flag(int b) {
    switch (b) {
        case 1: return {RI_MOUSE_LEFT_BUTTON_DOWN,  RI_MOUSE_LEFT_BUTTON_UP};
        case 2: return {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP};
        case 3: return {RI_MOUSE_MIDDLE_BUTTON_DOWN,RI_MOUSE_MIDDLE_BUTTON_UP};
        case 4: return {RI_MOUSE_BUTTON_4_DOWN,     RI_MOUSE_BUTTON_4_UP};
        case 5: return {RI_MOUSE_BUTTON_5_DOWN,     RI_MOUSE_BUTTON_5_UP};
    }
    return {0, 0};
}

// 出厂默认(X1/X2 由 config.h 决定),运行期被 mouse2controller.ini 覆盖
inline TriggerKey default_trigger() {
    return {TriggerKey::Mouse, (cfg::TRIGGER_BUTTON == 0) ? 4 : 5};
}

// 切挡键出厂默认:滚轮(上滚=上一挡、下滚=下一挡)。运行期被 ini [shift] 覆盖。
inline TriggerKey default_shift() {
    return {TriggerKey::Wheel, 0};
}

// 修饰键(左/右 Ctrl/Shift/Alt/Win):捕获时单独按下不结束捕获
inline bool is_modifier_vk(int vk) {
    switch (vk) {
        case VK_LSHIFT: case VK_RSHIFT:
        case VK_LCONTROL: case VK_RCONTROL:
        case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
            return true;
    }
    return false;
}

// 触发键的人类可读名字(UI 显示 / 控制台打印)
inline std::wstring trigger_name(const TriggerKey& tk) {
    if (tk.kind == TriggerKey::Wheel) return L"滚轮";
    if (tk.kind == TriggerKey::Mouse) {
        switch (tk.code) {
            case 1: return L"鼠标 左键";
            case 2: return L"鼠标 右键";
            case 3: return L"鼠标 中键";
            case 4: return L"鼠标 侧键 X1";
            case 5: return L"鼠标 侧键 X2";
        }
        return L"鼠标 ?";
    }
    wchar_t buf[64] = {};
    LONG scan = (LONG)MapVirtualKeyW(tk.code, MAPVK_VK_TO_VSC) << 16;
    if (GetKeyNameTextW(scan, buf, 64) && buf[0]) return buf;
    wchar_t fb[32];
    swprintf(fb, 32, L"键 0x%02X", tk.code);
    return fb;
}

// wstring -> UTF-8 窄串(配合 SetConsoleOutputCP(CP_UTF8) 用 printf 输出中文)
inline std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &s[0], n, nullptr, nullptr);
    return s;
}

// 绑定捕获目标(同一时刻最多捕获一个绑定项:触发键或切挡键)
enum CaptureTarget { CAP_NONE, CAP_TRIGGER, CAP_SHIFT };

TriggerKey    g_trigger   = default_trigger();  // 触发键:摇杆模式开关(ini [trigger] 覆盖)
TriggerKey    g_shiftKey  = default_shift();    // 切挡键:挡位切换(默认滚轮;ini [shift] 覆盖)
CaptureTarget g_capturing = CAP_NONE;           // 绑定捕获态(CAP_NONE = 未捕获)
std::wstring  g_iniPath;                        // = exe 目录 + "mouse2controller.ini"

StickMapper g_mapper;
vigem::Pad  g_pad;
bool        g_sideDown = false;
bool        g_keys[4] = {false, false, false, false};  // W, S, A, D
HHOOK       g_kbHook  = nullptr;
bool        g_testMode = false;

// ---- st_mode:按键→摇杆推力可编程曲线(自定义页)----
enum class CurveMode : int { TimeCurve = 0, TapHold = 1 };  // 两种手势模式(页中页切换)
CurveMap    g_curve;
std::mutex  g_curveMutex;   // 保护 g_curve:UI 线程编辑 ↔ 输入线程 eval 的唯一共享 vector
bool        g_curveEnabled = false;       // 自定义页开关;关→走默认页恒定 g_mag
CurveMode   g_curveMode    = CurveMode::TapHold;
float       g_tapThreshold = 200.0f;      // 点按/长按阈值(ms)
float       g_tapTarget    = 0.50f;        // 点按输出的目标推力
float       g_pulseMs      = 120.0f;      // 点按脉冲持续时长(ms)
ULONGLONG   g_pressStart   = 0;           // 方向键首次按下(无→有方向)时 = GetTickCount64()(见 low_level_kb_proc)
float       g_pressDur     = 0.0f;        // WM_TIMER 每帧刷新(按住累计时长 ms)
bool        g_tapActive    = false;       // 点按脉冲进行中
ULONGLONG   g_tapEnd       = 0;           // 脉冲到期时刻
float       g_tapDirX = 0.0f, g_tapDirY = 0.0f;     // 松开瞬间冻结的归一化方向
float       g_lastDirX = 0.0f, g_lastDirY = 0.0f;   // compute_target 最近一次方向快照

// ---- 默认页:推力挡位(滚轮在挡位间顺序切换)----
GearList   g_gears;           // 挡位列表(UI 改 ↔ 输入线程读)
std::mutex g_gearMutex;       // 保护 g_gears.values 的增删改查 vs 滚轮读取
int        g_gearIdx    = -1; // 当前激活挡位(-1 = 无);滚轮切换的对象
int        g_wheelAccum = 0;  // 滚轮残余累积(高精度滚轮用);WM_INPUT 单线程访问
std::atomic<bool> g_gearsHovered{false}; // 鼠标是否悬停挡位列表(UI 写 ↔ wheel_gear 读;滚轮切挡/滚页分工)

bool  g_lastSide = false;
bool  g_lastKeys[4] = {false, false, false, false};
short g_lastLx = 0, g_lastLy = 0;

inline int key_index(int vk) {
    switch (vk) {
        case 'W': return 0;
        case 'S': return 1;
        case 'A': return 2;
        case 'D': return 3;
        default:  return -1;
    }
}

// 给游戏补发 keydown/keyup(注入)。带 scancode,否则某些键游戏识别不到。
void send_key_event(int vk, bool down) {
    INPUT inp{};
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = vk;
    inp.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    inp.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &inp, sizeof(INPUT));
}

// 侧键状态变化:g_keys 始终由钩子维护。
//  进摇杆:对当前按住的 WASD 补发 keyup,释放游戏键盘输入。
//  回键盘:对仍按住的 WASD 补发 keydown,无缝恢复键盘输入。
// 侧键 = 摇杆模式总开关(按住=开,松开=关)。曲线计时基准与点按/长按判定都在
// 「方向键」上(见 low_level_kb_proc),这里只管开关状态 + 给游戏补发 WASD。
void on_side_changed(bool down) {
    g_sideDown = down;
    g_tapActive = false;                     // 开关状态变化:取消进行中的点按脉冲
    const int vks[4] = {'W', 'S', 'A', 'D'};
    for (int i = 0; i < 4; i++) {
        if (g_keys[i]) send_key_event(vks[i], down ? false : true);
    }
}

// ---- 持久化:触发键存 exe 同目录的 mouse2controller.ini ----
// 解析失败/文件缺失/值非法 -> 回落出厂默认,永不崩溃。
void load_config() {
    g_iniPath = vigem::exe_dir_file(L"mouse2controller.ini");

    // ---- [curve] 曲线推力配置(st_mode)----
    wchar_t cb[64] = {};
    GetPrivateProfileStringW(L"curve", L"enabled",   L"0",    cb, 64, g_iniPath.c_str());
    g_curveEnabled = (_wtoi(cb) != 0);
    GetPrivateProfileStringW(L"curve", L"mode",      L"1",    cb, 64, g_iniPath.c_str());
    g_curveMode = (_wtoi(cb) == 0) ? CurveMode::TimeCurve : CurveMode::TapHold;
    GetPrivateProfileStringW(L"curve", L"Tmax",      L"2000", cb, 64, g_iniPath.c_str()); g_curve.Tmax = (float)_wtof(cb);
    GetPrivateProfileStringW(L"curve", L"tap",       L"200",  cb, 64, g_iniPath.c_str()); g_tapThreshold = (float)_wtof(cb);
    GetPrivateProfileStringW(L"curve", L"taptarget", L"0.5",  cb, 64, g_iniPath.c_str()); g_tapTarget = (float)_wtof(cb);
    GetPrivateProfileStringW(L"curve", L"pulse",     L"120",  cb, 64, g_iniPath.c_str()); g_pulseMs = (float)_wtof(cb);
    GetPrivateProfileStringW(L"curve", L"count",     L"0",    cb, 64, g_iniPath.c_str());
    int count = _wtoi(cb);
    g_curve.pts.clear();
    for (int i = 0; i < count; i++) {
        wchar_t key[8]; swprintf(key, 8, L"p%d", i);
        wchar_t line[64] = {};
        GetPrivateProfileStringW(L"curve", key, L"", line, 64, g_iniPath.c_str());
        float t = 0, m = 0; int it = 1;
        if (swscanf(line, L"%f,%f,%d", &t, &m, &it) == 3) {
            if (it < 0 || it > 2) it = 1;
            g_curve.pts.push_back({t, m, (Interp)it});
        }
    }
    if (g_curve.pts.empty()) g_curve.setDefault();
    else { g_curve.sortPts(); g_curve.selected = 0; }

    // ---- [gears] 推力挡位(默认页滚轮切换)----
    // 放在 [trigger] 之前:trigger 块匹配成功会 return,挡位必须先加载完。
    GetPrivateProfileStringW(L"gears", L"count", L"0", cb, 64, g_iniPath.c_str());
    {
        int gcount = _wtoi(cb);
        if (gcount < 0) gcount = 0;
        if (gcount > GearList::MAX) gcount = GearList::MAX;
        g_gears.values.clear();
        for (int i = 0; i < gcount; i++) {
            wchar_t gkey[8]; swprintf(gkey, 8, L"g%d", i);
            wchar_t gline[32] = {};
            GetPrivateProfileStringW(L"gears", gkey, L"", gline, 32, g_iniPath.c_str());
            float v = (float)_wtof(gline);
            if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;   // 放开 0.31 下限,范围 [0,1]
            g_gears.values.push_back(v);
        }
        g_gears.selected = g_gears.empty() ? -1 : 0;

        GetPrivateProfileStringW(L"gears", L"active", L"-1", cb, 64, g_iniPath.c_str());
        int a = _wtoi(cb);
        if (a < 0 || a >= g_gears.size()) a = g_gears.empty() ? -1 : 0;
        g_gearIdx = a;

        GetPrivateProfileStringW(L"gears", L"mag", L"", cb, 64, g_iniPath.c_str());
        if (cb[0]) {
            float m = (float)_wtof(cb);
            if (m < 0.0f) m = 0.0f; if (m > 1.0f) m = 1.0f;
            g_mag = m;
        } else if (g_gearIdx >= 0) {
            g_mag = g_gears.values[g_gearIdx];   // 无 mag 记录则落到激活挡位
        }
    }

    // ---- [shift] 切挡键(挡位切换绑定,默认滚轮)----
    // 须在 [trigger] 块之前:trigger 块命中会提前 return,放后面就加载不到。
    {
        wchar_t skind[16] = {}, scode[16] = {};
        GetPrivateProfileStringW(L"shift", L"kind", L"", skind, 16, g_iniPath.c_str());
        GetPrivateProfileStringW(L"shift", L"code", L"", scode, 16, g_iniPath.c_str());
        if (wcscmp(skind, L"Wheel") == 0) {
            g_shiftKey = {TriggerKey::Wheel, 0};
        } else if (wcscmp(skind, L"Mouse") == 0) {
            int b = _wtoi(scode);
            g_shiftKey = (b >= 1 && b <= 5) ? TriggerKey{TriggerKey::Mouse, b} : default_shift();
        } else if (wcscmp(skind, L"Key") == 0) {
            int vk = _wtoi(scode);
            // 排除 WASD/Q(方向键/退出键),否则与核心功能冲突
            g_shiftKey = (vk > 0 && vk < 256 && key_index(vk) < 0 && vk != 'Q' && vk != 'q')
                         ? TriggerKey{TriggerKey::Key, vk} : default_shift();
        } else {
            g_shiftKey = default_shift();   // 缺失/非法 -> 回落出厂默认(滚轮)
        }
    }

    wchar_t kind[16] = {}, code[16] = {};
    GetPrivateProfileStringW(L"trigger", L"kind", L"", kind, 16, g_iniPath.c_str());
    GetPrivateProfileStringW(L"trigger", L"code", L"", code, 16, g_iniPath.c_str());
    if (wcscmp(kind, L"Mouse") == 0) {
        int b = _wtoi(code);
        if (b >= 1 && b <= 5) { g_trigger = {TriggerKey::Mouse, b}; return; }
    } else if (wcscmp(kind, L"Key") == 0) {
        int vk = _wtoi(code);
        if (vk > 0 && vk < 256 && key_index(vk) < 0 && vk != 'Q' && vk != 'q') {
            g_trigger = {TriggerKey::Key, vk};
            return;
        }
    }
    g_trigger = default_trigger();
}

void save_config() {
    if (g_iniPath.empty()) g_iniPath = vigem::exe_dir_file(L"mouse2controller.ini");
    const wchar_t* kind = (g_trigger.kind == TriggerKey::Mouse) ? L"Mouse" : L"Key";
    wchar_t code[16];
    swprintf(code, 16, L"%d", g_trigger.code);
    WritePrivateProfileStringW(L"trigger", L"kind", kind, g_iniPath.c_str());
    WritePrivateProfileStringW(L"trigger", L"code", code, g_iniPath.c_str());

    // ---- [shift] 切挡键(挡位切换绑定)----
    const wchar_t* skind = (g_shiftKey.kind == TriggerKey::Wheel) ? L"Wheel"
                           : (g_shiftKey.kind == TriggerKey::Mouse) ? L"Mouse" : L"Key";
    wchar_t scode[16];
    swprintf(scode, 16, L"%d", g_shiftKey.code);
    WritePrivateProfileStringW(L"shift", L"kind", skind, g_iniPath.c_str());
    WritePrivateProfileStringW(L"shift", L"code", scode, g_iniPath.c_str());

    // ---- [curve] ----
    WritePrivateProfileStringW(L"curve", L"enabled", g_curveEnabled ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"curve", L"mode",
        g_curveMode == CurveMode::TimeCurve ? L"0" : L"1", g_iniPath.c_str());
    wchar_t b[32];
    swprintf(b, 32, L"%.0f", g_curve.Tmax);       WritePrivateProfileStringW(L"curve", L"Tmax", b, g_iniPath.c_str());
    swprintf(b, 32, L"%.0f", g_tapThreshold);     WritePrivateProfileStringW(L"curve", L"tap", b, g_iniPath.c_str());
    swprintf(b, 32, L"%.2f", g_tapTarget);        WritePrivateProfileStringW(L"curve", L"taptarget", b, g_iniPath.c_str());
    swprintf(b, 32, L"%.0f", g_pulseMs);          WritePrivateProfileStringW(L"curve", L"pulse", b, g_iniPath.c_str());
    swprintf(b, 32, L"%d", (int)g_curve.pts.size());
    WritePrivateProfileStringW(L"curve", L"count", b, g_iniPath.c_str());
    for (int i = 0; i < (int)g_curve.pts.size(); i++) {
        wchar_t key[8]; swprintf(key, 8, L"p%d", i);
        wchar_t line[64];
        swprintf(line, 64, L"%.0f,%.2f,%d", g_curve.pts[i].t_ms, g_curve.pts[i].mag, (int)g_curve.pts[i].interp);
        WritePrivateProfileStringW(L"curve", key, line, g_iniPath.c_str());
    }

    // ---- [gears] 推力挡位 ----
    {
        static int s_prevGearCount = 0;   // 上次写入的 count,用于清理残键
        swprintf(b, 32, L"%d", g_gears.size());
        WritePrivateProfileStringW(L"gears", L"count", b, g_iniPath.c_str());
        for (int i = 0; i < g_gears.size(); i++) {
            wchar_t gkey[8]; swprintf(gkey, 8, L"g%d", i);
            swprintf(b, 32, L"%.3f", g_gears.values[i]);
            WritePrivateProfileStringW(L"gears", gkey, b, g_iniPath.c_str());
        }
        // 清理残键:i ∈ [新count, 上次count) 写空串删除(规避原 [curve] 已知坑)
        for (int i = g_gears.size(); i < s_prevGearCount; i++) {
            wchar_t gkey[8]; swprintf(gkey, 8, L"g%d", i);
            WritePrivateProfileStringW(L"gears", gkey, nullptr, g_iniPath.c_str());
        }
        s_prevGearCount = g_gears.size();

        swprintf(b, 32, L"%d", g_gearIdx);
        WritePrivateProfileStringW(L"gears", L"active", b, g_iniPath.c_str());
        swprintf(b, 32, L"%.3f", g_mag);
        WritePrivateProfileStringW(L"gears", L"mag", b, g_iniPath.c_str());
    }
}

// [诊断] 延迟日志:printf 打到控制台(-d 可见);文件 mouse2controller_lag.log 仅 -d 时写。
// fopen 只在异常事件(>=20ms)触发,频率低,对测量本身无实质干扰。
void lag_log(const char* s) {
    std::printf("%s", s);
    if (g_debugLog)
        if (FILE* f = std::fopen("mouse2controller_lag.log", "a")) { std::fputs(s, f); std::fclose(f); }
}

// 低级键盘钩子。判断顺序(不可乱序):
//   1) 注入放行  2) 绑定捕获  3) 键盘触发键检测  4) 记录 WASD  5) 摇杆模式吞 WASD
LRESULT CALLBACK low_level_kb_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        const int vk = static_cast<int>(info->vkCode);

        // [诊断探针1] info->time 是按键物理发生的时间戳(与 GetTickCount 同基准)。
        // dt = "按键产生 → mouse2controller 钩子被调用" 的端到端延迟。正常 <10ms。
        // 全程同步链:dt 飙高 = mouse2controller 这条 LL 钩子在阻塞全局键盘输入。
        const DWORD lag_dt = GetTickCount() - info->time;
        if (lag_dt >= 20) {
            char lb[160];
            std::snprintf(lb, sizeof(lb),
                "[KEY-LAG] %lums vk=0x%02X inj=%d fl=0x%X\n",
                (unsigned long)lag_dt, (unsigned)vk,
                (info->flags & LLKHF_INJECTED) ? 1 : 0, (unsigned)info->flags);
            lag_log(lb);
        }

        // 1) 注入输入放行(on_side_changed 补发的 WASD 不能被自己吞)
        if (info->flags & LLKHF_INJECTED) {
            return CallNextHookEx(g_kbHook, code, wp, lp);
        }

        const bool down = (wp == WM_KEYDOWN   || wp == WM_SYSKEYDOWN);
        const bool up   = (wp == WM_KEYUP     || wp == WM_SYSKEYUP);

        // 2) 绑定捕获模式:只认 keydown(触发键/切挡键共用,按 g_capturing 目标分流写入)
        if (g_capturing != CAP_NONE && down) {
            if (vk == VK_ESCAPE) {
                ui::on_capture_cancel();                              // 纯取消,不改绑定、不写 ini
                return 1;
            }
            if (is_modifier_vk(vk)) {
                return CallNextHookEx(g_kbHook, code, wp, lp);        // 单按修饰键:放行,继续等
            }
            if (key_index(vk) >= 0 || vk == 'Q' || vk == 'q') {
                return 1;                                             // WASD/Q:静默忽略,保持捕获
            }
            TriggerKey bind{TriggerKey::Key, vk};                    // 接受为键盘绑定
            if (g_capturing == CAP_SHIFT) g_shiftKey = bind;
            else                          g_trigger  = bind;
            save_config();
            ui::on_capture_done();
            return 1;                                                 // 吞掉这次 keydown
        }

        // 3) 键盘触发键检测:吞掉,让游戏收不到(键盘触发键会"占用"该键)
        if (g_trigger.kind == TriggerKey::Key && vk == g_trigger.code) {
            if (down) on_side_changed(true);
            else if (up) on_side_changed(false);
            return 1;
        }

        // 3b) 切挡键(键盘):单击下一挡(循环)。不吞键 —— 切挡是附加动作,游戏照常收键盘事件。
        //     若与触发键绑同一键,上面 3) 已 return,不会走到这里,故不冲突。
        if (g_shiftKey.kind == TriggerKey::Key && vk == g_shiftKey.code && down) {
            ui::shift_gear(+1, /*wrap=*/true);
        }

        // 4) WASD 状态 + 曲线计时基准(以「方向键按下」为起点,松开再按从慢开始)
        const int idx = key_index(vk);
        if (idx >= 0) {
            const bool wasAny = g_keys[0] || g_keys[1] || g_keys[2] || g_keys[3];
            if (down && !g_keys[idx]) {                  // 方向键按下(非自动重复)
                if (!wasAny) { g_pressStart = GetTickCount64(); g_tapActive = false; }  // 无→有方向:重置曲线
                g_keys[idx] = true;
            } else if (up) {
                g_keys[idx] = false;
                const bool nowAny = g_keys[0] || g_keys[1] || g_keys[2] || g_keys[3];
                // 有→无方向:点按/长按模式下,短按(< 阈值)触发一次点按脉冲(冻结方向)
                if (wasAny && !nowAny && g_sideDown && g_curveEnabled
                    && g_curveMode == CurveMode::TapHold && g_pressDur < g_tapThreshold) {
                    g_tapActive = true;
                    g_tapEnd    = GetTickCount64() + (ULONGLONG)g_pulseMs;
                    g_tapDirX   = g_lastDirX;
                    g_tapDirY   = g_lastDirY;
                }
            }
            // 5) 摇杆模式吞 WASD 的 keydown,让游戏收不到键盘方向键
            if (g_sideDown && down) return 1;
        }
    }
    return CallNextHookEx(g_kbHook, code, wp, lp);
}

// st_mode:取当前应输出的推力标量(替代恒定 g_mag)
inline float curve_current_mag() {
    const ULONGLONG now = GetTickCount64();
    if (g_curveMode == CurveMode::TapHold && g_tapActive) {   // 仅「点按/长按」模式有脉冲
        if (now < g_tapEnd) return g_tapTarget;
        g_tapActive = false;
    }
    if (g_sideDown) {                                         // 两模式共享:按住时沿曲线推进
        std::lock_guard<std::mutex> lk(g_curveMutex);         // 与 UI 编辑曲线互斥
        return g_curve.eval(g_pressDur);
    }
    return 0.0f;
}

void compute_target(float& tx, float& ty) {
    tx = 0.0f;
    ty = 0.0f;
    if (g_testMode) { ty = -1.0f; return; }  // 测试模式:满推 W
    if (!g_sideDown && !(g_curveEnabled && g_tapActive)) return;  // 脉冲在侧键松开后,须继续输出

    float dx, dy;
    if (g_curveEnabled && g_curveMode == CurveMode::TapHold && g_tapActive) {
        dx = g_tapDirX; dy = g_tapDirY;                       // 脉冲期间冻结方向
    } else {
        const bool w = g_keys[0], s = g_keys[1], a = g_keys[2], d = g_keys[3];
        dx = (d ? 1.0f : 0.0f) - (a ? 1.0f : 0.0f);
        dy = (s ? 1.0f : 0.0f) - (w ? 1.0f : 0.0f);
        const float mag = std::sqrt(dx * dx + dy * dy);
        if (mag <= 0.0001f) return;                          // 无方向,输出 0
        dx /= mag; dy /= mag;
    }
    g_lastDirX = dx; g_lastDirY = dy;                        // 方向快照(供下次点按脉冲)

    // 顺时针旋转 g_rotate 度(屏幕坐标 y 向下,正角 = 顺时针)
    const float rad = g_rotate * 3.14159265358979f / 180.0f;
    const float cs = std::cos(rad), sn = std::sin(rad);
    float rx = dx * cs - dy * sn;
    float ry = dx * sn + dy * cs;

    // 推力来源:曲线模式走曲线,否则恒定 g_mag
    const float drive = g_curveEnabled ? curve_current_mag() : g_mag;
    tx = rx * drive;
    ty = ry * drive;
}

void dbg_print(bool side, const bool k[4], short lx, short ly) {
    if (!side) {
        g_lastSide = false;
        g_lastLx = 0; g_lastLy = 0;
        g_lastKeys[0] = g_lastKeys[1] = g_lastKeys[2] = g_lastKeys[3] = false;
        return;
    }
    const bool changed = side != g_lastSide || lx != g_lastLx || ly != g_lastLy ||
                         k[0] != g_lastKeys[0] || k[1] != g_lastKeys[1] ||
                         k[2] != g_lastKeys[2] || k[3] != g_lastKeys[3];
    if (!changed) return;
    g_lastSide = side;
    g_lastKeys[0]=k[0]; g_lastKeys[1]=k[1]; g_lastKeys[2]=k[2]; g_lastKeys[3]=k[3];
    g_lastLx = lx; g_lastLy = ly;
    char keys[6]; int n = 0;
    if (k[0]) keys[n++] = 'W';
    if (k[1]) keys[n++] = 'S';
    if (k[2]) keys[n++] = 'A';
    if (k[3]) keys[n++] = 'D';
    keys[n] = 0;
    std::printf("[side=%d keys=%-4s lx=%6d ly=%6d]\n", side ? 1 : 0, keys, lx, ly);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INPUT: {
            UINT size = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, nullptr, &size,
                            sizeof(RAWINPUTHEADER));
            if (size == 0) break;
            static BYTE buffer[64];
            if (size > sizeof(buffer)) break;
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, buffer, &size,
                                sizeof(RAWINPUTHEADER)) == UINT(-1))
                break;
            auto* raw = reinterpret_cast<RAWINPUT*>(buffer);
            if (raw->header.dwType != RIM_TYPEMOUSE) break;
            const USHORT flags = raw->data.mouse.usButtonFlags;

            // 绑定捕获:任意鼠标键 DOWN 或滚轮(CAP_SHIFT 时) -> 接受为对应绑定
            if (g_capturing != CAP_NONE) {
                // 滚轮:仅切挡键捕获支持绑滚轮(触发键不支持滚轮)
                if (flags & RI_MOUSE_WHEEL) {
                    if (g_capturing == CAP_SHIFT) {
                        g_shiftKey = {TriggerKey::Wheel, 0};
                        save_config();
                        ui::on_capture_done();
                    }
                    break;   // 捕获态吞掉滚轮,不落到 wheel_gear
                }
                for (int b = 1; b <= 5; b++) {
                    if (flags & mouse_flag(b).down) {
                        TriggerKey bind{TriggerKey::Mouse, b};
                        if (g_capturing == CAP_SHIFT) g_shiftKey = bind;
                        else                          g_trigger  = bind;
                        save_config();
                        ui::on_capture_done();
                        break;  // 命中即结束捕获
                    }
                }
                break;  // 捕获态:其余鼠标事件一律忽略
            }

            // 鼠标触发键检测(Raw Input 只读,不占用按键)
            if (g_trigger.kind == TriggerKey::Mouse) {
                MouseFlagPair fp = mouse_flag(g_trigger.code);
                if (flags & fp.down) on_side_changed(true);
                if (flags & fp.up)   on_side_changed(false);
            }
            // 切挡键=鼠标键:单击下一挡(循环)。不吞键(附加动作,游戏照常收该鼠标键)
            if (g_shiftKey.kind == TriggerKey::Mouse) {
                MouseFlagPair sfp = mouse_flag(g_shiftKey.code);
                if (flags & sfp.down) ui::shift_gear(+1, /*wrap=*/true);
            }
            // 滚轮切挡位:仅当切挡键绑定为滚轮时(否则滚轮不参与切挡)
            if ((flags & RI_MOUSE_WHEEL) && g_shiftKey.kind == TriggerKey::Wheel)
                ui::wheel_gear((short)raw->data.mouse.usButtonData);
            break;
        }
        case WM_TIMER:
            if (wp == 1) {
                // [诊断探针2] 输入工作线程调度延迟:WM_TIMER 由 SetTimer 设为 8ms 一次。
                // 实测间隔 >20ms = 输入线程被节流(本线程独占 LL 钩子 + Raw Input + ViGEm,必然跟着慢)。
                static DWORD s_lastTimer = 0;
                const DWORD tnow = GetTickCount();
                if (s_lastTimer) {
                    const DWORD tgap = tnow - s_lastTimer;
                    if (tgap >= 20) {
                        char tb[96];
                        std::snprintf(tb, sizeof(tb),
                            "[TIMER-GAP] %lums (预期~8)\n", (unsigned long)tgap);
                        lag_log(tb);
                    }
                }
                s_lastTimer = tnow;
                if (g_sideDown && (g_keys[0] || g_keys[1] || g_keys[2] || g_keys[3]))
                    g_pressDur = (float)(GetTickCount64() - g_pressStart);
                float tx, ty;
                compute_target(tx, ty);
                g_mapper.set_target(tx, ty);
                short lx = 0, ly = 0;
                // 点按脉冲期间强制平滑系数 1.0 直冲,避免被 g_smooth 抹平
                const float sp = (g_curveEnabled && g_curveMode == CurveMode::TapHold && g_tapActive) ? 1.0f : -1.0f;
                g_mapper.tick(lx, ly, sp);
                g_pad.update_left_stick(lx, ly);
                dbg_print(g_sideDown, g_keys, lx, ly);
            } else if (wp == 2) {
                // 定期重装 LL 键盘钩子:Win10 Alt+Tab 切窗口后,系统可能因 LowLevelHooksTimeout
                // 静默摘除钩子,导致 WASD 不再被捕获/吞。重装自愈。
                if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
                g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, low_level_kb_proc, GetModuleHandleW(nullptr), 0);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND create_msg_window(HINSTANCE hInst) {
    const wchar_t* cls = L"mouse2controllerMsg";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    return CreateWindowExW(0, cls, L"mouse2controller", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                           hInst, nullptr);
}

void register_mouse(HWND hwnd) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

BOOL WINAPI ctrl_handler(DWORD t) {
    if (t == CTRL_C_EVENT || t == CTRL_CLOSE_EVENT) g_pad.release();
    return TRUE;
}

// 输入工作线程:独占 LL 键盘钩子 + Raw Input 鼠标 + SetTimer 摇杆更新 + ViGEm 发送。
// 关键——与主线程的 ImGui/D3D 渲染彻底隔离:全屏游戏+OBS 下主线程会被 Present 阻塞
// 数百 ms,但那碰不到本线程,所以键盘钩子始终即时(见 low_level_kb_proc 的 [KEY-LAG] 探针)。
DWORD WINAPI input_thread(LPVOID) {
    timeBeginPeriod(1);                                            // 1ms 定时器分辨率
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HWND msgHwnd = create_msg_window(hInst);
    register_mouse(msgHwnd);
    SetTimer(msgHwnd, 1, cfg::UPDATE_INTERVAL_MS, nullptr);
    SetTimer(msgHwnd, 2, 3000, nullptr);  // 每 3 秒触发 WM_TIMER(wp==2):重装 LL 键盘钩子,防止切窗口后被系统静默摘除

    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, low_level_kb_proc, GetModuleHandleW(nullptr), 0);
    if (!g_kbHook) std::printf("[警告] 键盘钩子安装失败\n");
    else           std::printf("[OK] 输入线程就绪(LL 钩子 + Raw Input + ViGEm)\n");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 本线程拥有,自行清理
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
    if (msgHwnd) DestroyWindow(msgHwnd);
    timeEndPeriod(1);
    return 0;
}

}  // namespace

// ===================== 设置窗口(ImGui + DirectX9) =====================
namespace ui {

// 自绘标题栏高度:WM_NCHITTEST 的拖动区 与 render_frame 的绘制区 共享,保证对齐
static constexpr float TITLEBAR_H = 30.0f;

// ---- D3D9 / ImGui 内部状态 ----
static LPDIRECT3D9           g_pD3D = nullptr;
static LPDIRECT3DDEVICE9     g_pd3dDevice = nullptr;
static D3DPRESENT_PARAMETERS g_d3dpp{};
static HWND                  g_hWnd = nullptr;

// 创建 D3D9 设备(硬件顶点处理失败则回落软件顶点处理)
static bool CreateDeviceD3D(HWND hWnd) {
    if ((g_pD3D = Direct3DCreate9(D3D_SDK_VERSION)) == nullptr) return false;
    ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
    g_d3dpp.Windowed = TRUE;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;  // 关 VSync,配合主循环 Sleep
    DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
    if (FAILED(g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, flags,
                                    &g_d3dpp, &g_pd3dDevice))) {
        flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        if (FAILED(g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, flags,
                                        &g_d3dpp, &g_pd3dDevice)))
            return false;
    }
    return true;
}

// 设备丢失/窗口尺寸变化时重置(字体 atlas 由 ImGui 自管,Reset 后无需重新 AddFont)
static void reset_device() {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    g_pd3dDevice->Reset(&g_d3dpp);
    ImGui_ImplDX9_CreateDeviceObjects();
}

// 切挡核心(steps 符号约定:正 = 下一挡/列表后一项,负 = 上一挡/前一项)。
// wrap=true 到边界绕回(按键单向循环);false 到边界停止(滚轮)。
// 曲线模式接管 g_mag 时让位(切挡无意义)。锁 g_gearMutex,与挡位列表编辑互斥。
void shift_gear(int steps, bool wrap) {
    if (g_curveEnabled) return;
    std::lock_guard<std::mutex> lk(g_gearMutex);
    if (g_gears.empty()) { g_gearIdx = -1; return; }
    const int n = g_gears.size();
    const int cur = (g_gearIdx >= 0 && g_gearIdx < n) ? g_gearIdx : 0;
    int next;
    if (wrap) next = ((cur + steps) % n + n) % n;        // 循环
    else {
        next = cur + steps;
        if (next < 0) next = 0;
        if (next >= n) next = n - 1;
    }
    g_gearIdx = next;
    g_mag = g_gears.values[g_gearIdx];   // 锁内读值 + 写 g_mag,保证一致
}

// 滚轮在推力挡位之间顺序切换(wheel_delta 通常 ±120/格 → 每格一挡)。
// 累积 ±120 算一挡(支持高精度滚轮的 <120 增量与多格滚动);残余留到下次。
// 上滚(wheel_delta>0)= 上一挡,下滚 = 下一挡;到边界停止;空列表无反应。
// 曲线模式接管 g_mag 时挡位滚轮让位(与默认页推力滑块灰禁语义一致)。
void wheel_gear(int wheel_delta) {
    if (g_curveEnabled) return;
    // 滚轮默认切挡(含游戏内)。仅当鼠标停在「设置窗口内、且不在挡位列表」时,把滚轮让给页面滚动。
    // 用 Win32 实时判断鼠标所在窗口(不依赖 ImGui hover —— 设置窗口被游戏覆盖不渲染时 hover 值会过期)。
    POINT _pt; GetCursorPos(&_pt);
    HWND  _hw = WindowFromPoint(_pt);
    const bool inSettings = hwnd() && (_hw == hwnd() || IsChild(hwnd(), _hw));
    if (inSettings && !g_gearsHovered.load(std::memory_order_relaxed)) return;
    g_wheelAccum += wheel_delta;
    int raw = g_wheelAccum / 120;
    if (raw == 0) return;                  // 残余不足一挡,继续累积
    g_wheelAccum -= raw * 120;
    shift_gear(-raw, /*wrap=*/false);      // 上滚(raw>0)→ steps 负 → 上一挡;滚轮非循环
}

// 捕获完成:仅复位捕获态(立即模式:按钮文本/当前值每帧自动反映,无需 SetWindowText)
void on_capture_done() {
    const CaptureTarget t = g_capturing;            // 先存目标再清(打印用)
    g_capturing = CAP_NONE;
    const TriggerKey& k = (t == CAP_SHIFT) ? g_shiftKey : g_trigger;
    std::printf("[已绑定] %s\n", to_utf8(trigger_name(k)).c_str());
}

// 捕获取消(ESC):只复位捕获态,不改 g_trigger、不写 ini
void on_capture_cancel() {
    g_capturing = CAP_NONE;
}

// hover/聚焦时用 Left/Right 微调刚绘制的 SliderFloat(Shift 大步)。紧跟 SliderFloat 后调用。
// SetItemKeyOwner 收归方向键,避免被其他控件/导航误吞(默认页 4 个滑块 + 挡位编辑滑块共用)。
static void nudge_float(float& v, float vmin, float vmax, float smallStep, float bigStep) {
    if (!ImGui::IsItemHovered() && !ImGui::IsItemFocused()) return;
    ImGui::SetItemKeyOwner(ImGuiKey_LeftArrow);
    ImGui::SetItemKeyOwner(ImGuiKey_RightArrow);
    const bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    const float step = shift ? bigStep : smallStep;
    bool moved = false;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  true)) { v -= step; moved = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) { v += step; moved = true; }
    if (moved) { if (v < vmin) v = vmin; if (v > vmax) v = vmax; }
}

// 圆盘方向指示器:InvisibleButton 占布局空间,ImDrawList 画十字+圆+红箭头,位置跟随布局
static void draw_dial() {
    const float R = 32.0f;
    const ImVec2 size(2 * R + 12, 2 * R + 12);
    ImGui::InvisibleButton("##dial", size);
    const ImVec2 pmin = ImGui::GetItemRectMin();
    const ImVec2 c(pmin.x + size.x * 0.5f, pmin.y + size.y * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 G = IM_COL32(160, 160, 160, 255);
    const ImU32 Rd = IM_COL32(220, 30, 30, 255);
    dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), G, 1.0f);  // 横
    dl->AddLine(ImVec2(c.x, c.y - R), ImVec2(c.x, c.y + R), G, 1.0f);  // 竖
    dl->AddCircle(c, R, G, 32, 1.50f);                                   // 空心圆
    const float rad = g_rotate * 3.14159265358979f / 180.0f;
    const float fx = std::sin(rad), fy = -std::cos(rad);               // 屏幕坐标 y 向下
    dl->AddLine(c, ImVec2(c.x + fx * (R - 3), c.y + fy * (R - 3)), Rd, 3.0f);  // 红箭头
}

// "默认"页:推力/平滑/回中/旋转滑块 + 圆盘 + 推力挡位
static void draw_default_tab() {
    // 整页放进可滚动 child:内容多时滚轮上下滚页(隐藏滚动条),Tab 栏固定不被带走。
    // 滚轮切挡只在鼠标悬停挡位列表时生效(见 ##gears_list / wheel_gear)。
    ImGui::BeginChild("##default_tab", ImGui::GetContentRegionAvail(), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted("推力 MAGNITUDE"); ImGui::SameLine(220); ImGui::Text("%.2f", g_mag);
    if (g_curveEnabled) { ImGui::SameLine(); ImGui::TextDisabled("  [曲线已接管]"); }
    ImGui::BeginDisabled(g_curveEnabled);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##mag", &g_mag, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    if (!g_curveEnabled) nudge_float(g_mag, 0.0f, 1.0f, 0.01f, 0.05f);   // hover 时 ←/→ 微调(Shift 大步)

    ImGui::TextUnformatted("平滑 SMOOTHING"); ImGui::SameLine(220); ImGui::Text("%.2f", g_smooth);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##smooth", &g_smooth, 0.0f, 1.0f, "%.2f");
    nudge_float(g_smooth, 0.0f, 1.0f, 0.01f, 0.05f);

    ImGui::TextUnformatted("回中 RETURN_SPEED"); ImGui::SameLine(220); ImGui::Text("%.2f", g_return);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##ret", &g_return, 0.0f, 1.0f, "%.2f");
    nudge_float(g_return, 0.0f, 1.0f, 0.01f, 0.05f);

    ImGui::TextUnformatted("旋转 ROTATE(顺时针 +)"); ImGui::SameLine(220); ImGui::Text("%.0f°", g_rotate);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##rot", &g_rotate, -90.0f, 90.0f, "%.1f");
    nudge_float(g_rotate, -90.0f, 90.0f, 1.0f, 5.0f);

    ImGui::Separator();
    draw_dial();
    ImGui::Separator();

    ImGui::TextWrapped("红箭头 = 旋转后的\"前\"方向。实时生效;");

    // ---- 推力挡位(滚轮在挡位间顺序切换)----
    ImGui::Separator();
    ImGui::TextUnformatted("推力挡位(滚轮在挡位间顺序切换)");
    ImGui::TextWrapped("上/下滚轮在挡位间切换;空列表或第一/末挡时滚轮无反应。拖动行可排序;点行选中后可在下方改值,或点行尾\"删除\"直接删除。");

    // 添加当前推力为挡位
    {
        const bool canAdd = !g_curveEnabled && g_gears.size() < GearList::MAX;
        ImGui::BeginDisabled(!canAdd);
        if (ImGui::Button("添加当前推力为挡位", ImVec2(-1.0f, 0.0f))) {
            std::lock_guard<std::mutex> lk(g_gearMutex);
            g_gears.add(g_mag);   // 只入库,不改 g_gearIdx:当前激活挡位保持不变
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (g_gears.size() >= GearList::MAX) ImGui::TextDisabled("已达上限 %d", GearList::MAX);
        else                                 ImGui::TextDisabled("当前 %d / %d", g_gears.size(), GearList::MAX);
    }

    // 挡位列表(可滚动 + 拖动排序 + 行内删除)
    ImGui::BeginChild("##gears_list", ImVec2(0.0f, 120.0f), ImGuiChildFlags_Border);
    {
        std::lock_guard<std::mutex> lk(g_gearMutex);
        // 删除指定挡位并修正 g_gearIdx/g_mag(行内按钮与选中面板共用)
        auto deleteGear = [&](int i) {
            if (i < 0 || i >= g_gears.size()) return;
            const bool wasActive = (i == g_gearIdx);
            g_gears.removeAt(i);
            if (wasActive) {
                if (g_gears.empty()) g_gearIdx = -1;
                else {
                    if (g_gearIdx >= g_gears.size()) g_gearIdx = g_gears.size() - 1;
                    g_mag = g_gears.values[g_gearIdx];
                }
            } else if (g_gearIdx > i) {
                g_gearIdx--;   // 删除点在激活位之前:激活索引左移一位
            }
        };
        const float btnW = ImGui::CalcTextSize("删除").x + ImGui::GetStyle().FramePadding.x * 2.0f
                         + ImGui::GetStyle().ItemSpacing.x;
        for (int i = 0; i < g_gears.size(); i++) {
            const bool isActive = (i == g_gearIdx);
            const bool isSel    = (i == g_gears.selected);
            char label[64];
            std::snprintf(label, sizeof(label), "%s %2d   |   %.2f%s",
                          isActive ? ">>" : "  ", i + 1, g_gears.values[i],
                          isActive ? "   (激活)" : "");
            const float selW = ImGui::GetContentRegionAvail().x - btnW;
            if (isSel) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.35f, 0.45f, 0.75f, 0.60f));
            if (ImGui::Selectable(label, isSel, ImGuiSelectableFlags_None,
                                  ImVec2(selW > 80.0f ? selW : 80.0f, 0.0f)))
                g_gears.selected = i;
            if (isSel) ImGui::PopStyleColor();

            // 拖动重排(imgui_demo 列表 reorder 范式):拖出本行超过 ±1 行高则与邻位交换
            if (ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
                const float dy = ImGui::GetMouseDragDelta(0).y;
                const float th = ImGui::GetFrameHeight();
                int i_next = i + ((dy < -th) ? -1 : (dy > th) ? +1 : 0);
                if (i_next >= 0 && i_next < g_gears.size()) {
                    g_gears.swap(i, i_next);
                    if (g_gears.selected == i)          g_gears.selected = i_next;
                    else if (g_gears.selected == i_next) g_gears.selected = i;
                    if (g_gearIdx == i)                 g_gearIdx = i_next;
                    else if (g_gearIdx == i_next)       g_gearIdx = i;
                    ImGui::ResetMouseDragDelta();
                }
            }

            // 行内删除按钮(直接删除,无需先选中)
            ImGui::SameLine();
            ImGui::PushID(i);
            if (ImGui::SmallButton("删除")) deleteGear(i);
            ImGui::PopID();
        }
    }
    g_gearsHovered.store(ImGui::IsWindowHovered(), std::memory_order_relaxed);  // 鼠标是否悬停挡位列表(滚轮切挡分工)
    ImGui::EndChild();

    // 选中挡位编辑面板(套曲线编辑器选中面板范式)
    if (g_gears.selected >= 0 && g_gears.selected < g_gears.size()) {
        std::lock_guard<std::mutex> lk(g_gearMutex);
        const int sel = g_gears.selected;
        ImGui::Text("选中挡位 #%d", sel + 1);
        ImGui::SetNextItemWidth(-1);
        float* vp = &g_gears.values[sel];
        ImGui::SliderFloat("##gear_val", vp, 0.0f, 1.0f, "%.3f");   // 直接绑定挡位值
        nudge_float(*vp, 0.0f, 1.0f, 0.005f, 0.02f);                // hover 时 ←/→ 微调该挡位
        if (sel == g_gearIdx) g_mag = *vp;                          // 改的恰好是激活挡位:实时同步 g_mag
        ImGui::Spacing();
        if (ImGui::Button("设为激活")) g_gearIdx = sel;
        ImGui::SameLine();
        if (ImGui::Button("删除该挡位")) {
            const bool wasActive = (sel == g_gearIdx);
            g_gears.removeAt(sel);
            if (wasActive) {
                if (g_gears.empty()) g_gearIdx = -1;
                else {
                    if (g_gearIdx >= g_gears.size()) g_gearIdx = g_gears.size() - 1;
                    g_mag = g_gears.values[g_gearIdx];
                }
            } else if (g_gearIdx > sel) {
                g_gearIdx--;   // 删除点在激活位之前:激活索引左移一位
            }
        }
    } else {
        ImGui::TextDisabled("(未选中挡位 — 点列表行可选中编辑)");
    }
    ImGui::EndChild();
}

// "快捷键"页:触发键(摇杆开关)+ 切挡键(挡位切换)绑定,均可自定义到鼠标键/键盘键/滚轮。
static void draw_hotkeys_tab() {
    ImGui::BeginChild("##hotkeys_tab", ImGui::GetContentRegionAvail(), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

    // ---- 触发键(摇杆模式开关:按住=进入摇杆,松开=回键盘)----
    ImGui::PushID("trig");   // 与切挡键的同名按钮(绑定…/恢复默认)区分 ID,避免 ImGui ID 冲突
    ImGui::TextUnformatted("触发键(摇杆模式开关,按住生效)");
    ImGui::Text("当前:%s", to_utf8(trigger_name(g_trigger)).c_str());
    const char* tbtn = (g_capturing == CAP_TRIGGER) ? "等待按键…(ESC 取消)" : "绑定…";
    if (ImGui::Button(tbtn)) g_capturing = CAP_TRIGGER;
    ImGui::SameLine();
    if (ImGui::Button("恢复默认")) { g_trigger = default_trigger(); save_config(); }
    ImGui::TextDisabled("可绑鼠标键(左/右/中/侧键 X1·X2)或任意键盘键(不含 WASD/Q)。");
    ImGui::PopID();

    ImGui::Separator();

    // ---- 切挡键(切换推力挡位)----
    ImGui::PushID("shift");   // 与触发键的同名按钮区分 ID
    ImGui::TextUnformatted("切挡键(切换推力挡位)");
    ImGui::Text("当前:%s", to_utf8(trigger_name(g_shiftKey)).c_str());
    const char* sbtn = (g_capturing == CAP_SHIFT) ? "等待按键/滚轮…(ESC 取消)" : "绑定…";
    if (ImGui::Button(sbtn)) g_capturing = CAP_SHIFT;
    ImGui::SameLine();
    if (ImGui::Button("设为滚轮")) { g_shiftKey = {TriggerKey::Wheel, 0}; save_config(); }
    ImGui::SameLine();
    if (ImGui::Button("恢复默认")) { g_shiftKey = default_shift(); save_config(); }
    ImGui::TextWrapped("绑滚轮:上/下滚双向切挡;绑鼠标键或键盘键:单击切下一挡(末挡循环回首挡)。"
                       "曲线推力模式下切挡自动失效;避免与触发键绑同一个键。");
    ImGui::PopID();

    ImGui::EndChild();
}

// ---- 曲线编辑器(自定义页两个子模式共享)----
// 画布:横轴 0..Tmax(ms),纵轴 0..1(推力)。鼠标左键添点/选点/拖动,右键删点;
//       选中点时方向键 ↑↓ 改推力、←→ 改时间(Shift 大步),Delete 删点。
static void draw_curve_editor() {
    ImGui::TextWrapped("左键:添点/选点/拖动   右键/Delete:删点   方向键微调(Shift 大步)");
    ImVec2 csz = ImGui::GetContentRegionAvail();
    csz.y = 200.0f;
    if (csz.x < 100.0f) csz.x = 100.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + csz.x, p0.y + csz.y);

    ImGui::InvisibleButton("##curve", csz,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();
    // 画布悬停时:把方向键/Delete 的所有权收归画布,避免被下面的输入框或导航抢走
    // (上下调推力、左右调时间,均只作用于选中点)
    if (hovered) {
        ImGui::SetItemKeyOwner(ImGuiKey_UpArrow);
        ImGui::SetItemKeyOwner(ImGuiKey_DownArrow);
        ImGui::SetItemKeyOwner(ImGuiKey_LeftArrow);
        ImGui::SetItemKeyOwner(ImGuiKey_RightArrow);
        ImGui::SetItemKeyOwner(ImGuiKey_Delete);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 背景与网格
    dl->AddRectFilled(p0, p1, IM_COL32(40, 40, 40, 255));
    dl->AddRect(p0, p1, IM_COL32(110, 110, 110, 255));
    const ImU32 gridCol = IM_COL32(70, 70, 70, 255);
    for (int i = 1; i < 4; i++) {
        float gy = p0.y + csz.y * i / 4.0f;
        dl->AddLine(ImVec2(p0.x, gy), ImVec2(p1.x, gy), gridCol, 1.0f);
        float gx = p0.x + csz.x * i / 4.0f;
        dl->AddLine(ImVec2(gx, p0.y), ImVec2(gx, p1.y), gridCol, 1.0f);
    }

    // 坐标转换(y 轴翻转:推力向上)
    auto toPx = [&](float t, float m) {
        return ImVec2(p0.x + (t / g_curve.Tmax) * csz.x, p0.y + (1.0f - m) * csz.y);
    };
    auto fromPx = [&](ImVec2 m, float& t, float& mm) {
        t  = ((m.x - p0.x) / csz.x) * g_curve.Tmax;
        mm = 1.0f - ((m.y - p0.y) / csz.y);
        if (t < 0) t = 0; if (t > g_curve.Tmax) t = g_curve.Tmax;
        if (mm < 0) mm = 0; if (mm > 1) mm = 1;
    };

    // 曲线折线(按像素采样 eval)
    const ImU32 curveCol = IM_COL32(80, 180, 255, 255);
    for (float px = 0; px < csz.x - 2.0f; px += 2.0f) {
        float ta = (px / csz.x) * g_curve.Tmax;
        float tb = ((px + 2.0f) / csz.x) * g_curve.Tmax;
        dl->AddLine(toPx(ta, g_curve.eval(ta)), toPx(tb, g_curve.eval(tb)), curveCol, 2.0f);
    }

    // 当前 press_duration 竖线(运行时反馈)
    if (g_sideDown) {
        float vx = p0.x + (g_pressDur / g_curve.Tmax) * csz.x;
        if (vx > p1.x) vx = p1.x;
        dl->AddLine(ImVec2(vx, p0.y), ImVec2(vx, p1.y), IM_COL32(255, 220, 60, 220), 1.50f);
    }

    // 命中检测:平方距离最近的点(~14px 半径)
    auto pickPoint = [&](ImVec2 m) -> int {
        int best = -1; float bd = 14.0f * 14.0f;
        for (int i = 0; i < (int)g_curve.pts.size(); i++) {
            ImVec2 c = toPx(g_curve.pts[i].t_ms, g_curve.pts[i].mag);
            float d = (c.x - m.x) * (c.x - m.x) + (c.y - m.y) * (c.y - m.y);
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    };
    const ImVec2 mouse = ImGui::GetMousePos();

    {   // 编辑 pts(改 size / 重排)——与输入线程 eval 短暂互斥
        std::lock_guard<std::mutex> lk(g_curveMutex);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {        // 右键删点
            int hit = pickPoint(mouse);
            if (hit >= 0) { g_curve.selected = hit; g_curve.removeSelected(); }
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {         // 左键:命中→选中,空白→添点
            int hit = pickPoint(mouse);
            if (hit >= 0) g_curve.selected = hit;
            else { float t, m; fromPx(mouse, t, m); g_curve.add(t, m); }
        }
        if (active && g_curve.selected >= 0 && g_curve.selected < (int)g_curve.pts.size()
            && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {                    // 拖动选中点
            float t, m; fromPx(mouse, t, m);
            g_curve.pts[g_curve.selected].t_ms = t;
            g_curve.pts[g_curve.selected].mag  = m;
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) g_curve.sortPts();
    }

    // 画控制点(选中高亮)
    for (int i = 0; i < (int)g_curve.pts.size(); i++) {
        ImVec2 c = toPx(g_curve.pts[i].t_ms, g_curve.pts[i].mag);
        ImU32 col = (i == g_curve.selected) ? IM_COL32(255, 200, 40, 255) : IM_COL32(220, 220, 220, 255);
        dl->AddRectFilled(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), col);
        if (i == g_curve.selected)
            dl->AddRect(ImVec2(c.x - 7, c.y - 7), ImVec2(c.x + 7, c.y + 7), IM_COL32(255, 255, 255, 255));
    }

    // 键盘微调(仅 hover 时;Shift 大步)
    if (hovered && g_curve.selected >= 0 && g_curve.selected < (int)g_curve.pts.size()) {
        std::lock_guard<std::mutex> lk(g_curveMutex);   // 改 pts —— 与输入线程 eval 互斥
        const bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
        const float dtT = shift ? 100.0f : 10.0f;
        const float dtM = shift ? 0.10f : 0.01f;
        CurvePoint& p = g_curve.pts[g_curve.selected];
        bool moved = false;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  true)) { p.t_ms -= dtT; moved = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) { p.t_ms += dtT; moved = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,    true)) { p.mag  += dtM; moved = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow,  true)) { p.mag  -= dtM; moved = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete,     false)) g_curve.removeSelected();
        if (moved) {
            if (p.t_ms < 0) p.t_ms = 0; if (p.t_ms > g_curve.Tmax) p.t_ms = g_curve.Tmax;
            if (p.mag  < 0) p.mag  = 0; if (p.mag  > 1) p.mag  = 1;
            g_curve.sortPts();
        }
    }

    // 选中点参数面板
    ImGui::Spacing();
    if (g_curve.selected >= 0 && g_curve.selected < (int)g_curve.pts.size()) {
        std::lock_guard<std::mutex> lk(g_curveMutex);   // 改 pts —— 与输入线程 eval 互斥
        CurvePoint& p = g_curve.pts[g_curve.selected];
        ImGui::Text("选中点 #%d", g_curve.selected);
        int it = (int)p.interp;
        ImGui::SetNextItemWidth(120);
        if (ImGui::Combo("插值", &it, "Linear\0Smooth\0Step\0")) p.interp = (Interp)it;
        ImGui::TextUnformatted("时间(ms)");
        ImGui::SetNextItemWidth(-1); ImGui::InputFloat("##pt_t", &p.t_ms, 1.0f, 10.0f, "%.0f");
        ImGui::TextUnformatted("推力");
        ImGui::SetNextItemWidth(-1); ImGui::InputFloat("##pt_m", &p.mag, 0.01f, 0.10f, "%.3f");
        if (ImGui::Button("删除该点")) g_curve.removeSelected();
        if (p.t_ms < 0) p.t_ms = 0; if (p.t_ms > g_curve.Tmax) p.t_ms = g_curve.Tmax;
        if (p.mag  < 0) p.mag  = 0; if (p.mag  > 1) p.mag  = 1;
    } else {
        ImGui::TextDisabled("(未选中点 — 左键点击曲线添加/选中)");
    }
}

// "自定义"页:启用开关 + 嵌套标签页(时间曲线 / 点按·长按)+ 全局参数
// 标签全部前置(控件用 ##id 占满宽度),避免右侧 label 被窗口裁切。
static void draw_custom_tab() {
    // 整页放进可滚动 child(隐藏滚动条,滚轮上下滚页),与默认页一致
    ImGui::BeginChild("##custom_tab", ImGui::GetContentRegionAvail(), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::Checkbox("启用曲线推力 (st_mode)", &g_curveEnabled);
    ImGui::TextWrapped("(未启用时使用默认页恒定推力)");

    if (ImGui::BeginTabBar("##mode")) {
        bool timeSel = (g_curveMode == CurveMode::TimeCurve);
        if (ImGui::BeginTabItem("时间曲线", &timeSel)) {
            g_curveMode = CurveMode::TimeCurve;
            draw_curve_editor();
            ImGui::TextWrapped("推力 = 曲线(按住时长);短按停在曲线早期值,无点按脉冲。");
            ImGui::EndTabItem();
        }
        bool tapSel = (g_curveMode == CurveMode::TapHold);
        if (ImGui::BeginTabItem("点按/长按", &tapSel)) {
            g_curveMode = CurveMode::TapHold;
            draw_curve_editor();
            ImGui::Separator();
            ImGui::TextUnformatted("点按参数(短按 < 阈值 时触发脉冲)");
            ImGui::TextUnformatted("点按阈值 tap(ms)");
            ImGui::SetNextItemWidth(-1); ImGui::InputFloat("##tap", &g_tapThreshold, 10.0f, 50.0f, "%.0f");
            ImGui::TextUnformatted("点按目标推力");
            ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##tapt", &g_tapTarget, 0.0f, 1.0f, "%.2f");
            ImGui::TextUnformatted("点按脉冲时长(ms)");
            ImGui::SetNextItemWidth(-1); ImGui::InputFloat("##pulse", &g_pulseMs, 10.0f, 50.0f, "%.0f");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("曲线时间上限 Tmax(ms)");
    ImGui::SetNextItemWidth(-1); ImGui::InputFloat("##tmax", &g_curve.Tmax, 100.0f, 500.0f, "%.0f");
    if (ImGui::Button("恢复默认曲线")) { std::lock_guard<std::mutex> lk(g_curveMutex); g_curve.setDefault(); }
    ImGui::SameLine();
    if (ImGui::Button("保存到 ini")) save_config();
    ImGui::EndChild();
}

// "关于"页:Powered by(顶部) + 免责声明(上) + 致谢/开源许可(下)
static void draw_about_tab() {
    ImGui::BeginChild("##about_tab", ImGui::GetContentRegionAvail(), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.80f, 1.00f, 1.00f));
    ImGui::TextUnformatted("Crafted by p2b");
    ImGui::SameLine();
    ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::TextUnformatted("免责声明");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(
        "本软件按「现状」提供,不附带任何明示或暗示的担保。"
        "因使用或无法使用本软件而产生的任何直接或间接损失,作者概不负责。"
        "请遵守所在地区法律及对应游戏 / 软件的服务条款,自行评估并承担使用风险。");
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("致谢 / 开源许可");
    ImGui::BulletText("过期薯条#5220");
    ImGui::BulletText("Dear ImGui  (MIT License)");
    ImGui::SameLine(); ImGui::TextDisabled("github.com/ocornut/imgui");
    ImGui::BulletText("ViGEmClient  (MIT License)");
    ImGui::SameLine(); ImGui::TextDisabled("github.com/nefarius/ViGEmClient");
    ImGui::BulletText("ViGEmBus  (BSD-3-Clause License)");
    ImGui::SameLine(); ImGui::TextDisabled("github.com/nefarius/ViGEmBus");
    ImGui::Spacing();
    ImGui::TextDisabled("感谢以上开源项目与本软件所依赖的驱动程序。");
    ImGui::EndChild();
}

// 每帧:构建 UI 并 Present(仅当设置窗口可见时由主循环调用)
void render_frame() {
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    g_gearsHovered.store(false, std::memory_order_relaxed);  // 每帧重置;默认页 ##gears_list 渲染时设回真值(避免切到其他 Tab 后残留旧值导致滚轮误切挡)

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoScrollbar);   // 隐藏滚动条(标题栏自绘,见下)

    // ---- 自绘标题栏(取代 Windows 系统标题栏;拖动靠 WM_NCHITTEST) ----
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + TITLEBAR_H),
                          IM_COL32(36, 36, 40, 255));
        const float ty = wp.y + (TITLEBAR_H - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(wp.x + 12.0f, ty), IM_COL32(228, 228, 232, 255), "Mouse2Controller");
        const float by = wp.y + (TITLEBAR_H - 22.0f) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(wp.x + ws.x - 78.0f, by));
        if (ImGui::Button("—", ImVec2(30.0f, 22.0f))) ShowWindow(g_hWnd, SW_MINIMIZE);
        ImGui::SetCursorScreenPos(ImVec2(wp.x + ws.x - 42.0f, by));
        if (ImGui::Button("×", ImVec2(34.0f, 22.0f))) PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        // 后续内容从标题栏下方 + 窗口 padding 开始
        const ImVec2 pad = ImGui::GetStyle().WindowPadding;
        ImGui::SetCursorScreenPos(ImVec2(wp.x + pad.x, wp.y + TITLEBAR_H + pad.y));
    }

    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("默认")) {
            draw_default_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("快捷键")) {
            draw_hotkeys_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("自定义")) {
            draw_custom_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("关于")) {
            draw_about_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    ImGui::Render();

    g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_RGBA(30, 30, 30, 255), 1.0f, 0);
    if (g_pd3dDevice->BeginScene() >= 0) {
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        g_pd3dDevice->EndScene();
    }
    if (g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST &&
        g_pd3dDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
        reset_device();
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return true;
    switch (msg) {
        case WM_NCHITTEST: {   // 无边框窗口:自管拖动(标题栏) + 边缘调大小
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(h, &pt);
            RECT rc; GetClientRect(h, &rc);
            const int E = 5;
            const bool L = pt.x < E, R = pt.x >= rc.right - E;
            const bool T = pt.y < E, B = pt.y >= rc.bottom - E;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
            if (pt.y < (int)TITLEBAR_H) {                   // 标题栏 → 拖动
                if (pt.x >= rc.right - 84) return HTCLIENT;  // 右上按钮区交给 ImGui
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_SIZE:
            if (g_pd3dDevice && wp != SIZE_MINIMIZED) {
                g_d3dpp.BackBufferWidth  = LOWORD(lp);
                g_d3dpp.BackBufferHeight = HIWORD(lp);
                reset_device();
            }
            return 0;
        case WM_GETMINMAXINFO: {  // 限制最小尺寸,防控件挤成一团
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = 360;
            mmi->ptMinTrackSize.y = 420;
            return 0;
        }
        case WM_CLOSE:
            if (g_capturing != CAP_NONE) g_capturing = CAP_NONE;
            save_config();                          // 保存曲线/触发键到 ini
            DestroyWindow(h);                       // 关闭窗口即退出程序
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND create(HINSTANCE hInst) {
    const wchar_t* cls = L"mouse2controllerSettings";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // 窗口 / 任务栏图标:从 exe 自身资源加载(IDI_APPICON 已由 resource.rc 编进 exe)
    wc.hIcon   = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.lpszClassName = cls;
    wc.style = CS_DROPSHADOW;   // 无边框窗口加细阴影,避免"飘"在桌面
    RegisterClassExW(&wc);
    // WS_POPUP 去掉系统标题栏/边框;标题栏、最小化/关闭按钮全部 ImGui 自绘,
    // 拖动与边缘调大小由 WM_NCHITTEST 处理。WS_EX_APPWINDOW 让它在任务栏正常显示。
    HWND h = CreateWindowExW(WS_EX_APPWINDOW, cls, L"Mouse2Controller",
                             WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             460, 520, nullptr, nullptr, hInst, nullptr);
    if (!CreateDeviceD3D(h)) {
        std::printf("[错误] 创建 D3D9 设备失败,设置窗口无法显示。\n");
        DestroyWindow(h);
        return nullptr;
    }

    // ImGui 上下文 + 中文字体(必须在 ImGui_ImplDX9_Init 之前加载)
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // 不启用键盘导航(NavEnableKeyboard):方向键留给曲线编辑器微调用,
    // 否则会被导航用来在控件间跳焦点、或被输入框抢去增减数值。
    io.IniFilename = nullptr;  // 不写 imgui.ini,避免污染 exe 目录
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18.0f, nullptr,
                                      io.Fonts->GetGlyphRangesChineseSimplifiedCommon())) {
        // 回落:黑体(Windows 必装)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", 18.0f, nullptr,
                                     io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }
    ImGui_ImplWin32_Init(h);
    ImGui_ImplDX9_Init(g_pd3dDevice);

    ShowWindow(h, SW_SHOW);
    g_hWnd = h;
    return h;
}

void shutdown() {
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    if (g_pD3D)       { g_pD3D->Release();       g_pD3D = nullptr; }
}

HWND hwnd() { return g_hWnd; }  // 供主循环查询可见性

}  // namespace ui

int main() {
    // 默认纯 ImGui(无控制台);命令行带 -d 才保留控制台用于调试,并启用 lag.log 落盘。尽早释放以减少闪烁。
    g_debugLog = wcsstr(GetCommandLineW(), L"-d") != nullptr;
    if (!g_debugLog) FreeConsole();
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"mouse2controller");
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    // 进程级提优先级。LL 键盘钩子已移到独立"输入工作线程"(input_thread),那里再自提
    // TIME_CRITICAL + timeBeginPeriod(1)。主线程只做 ImGui 渲染,不设 TIME_CRITICAL ——
    // 渲染的 Present 阻塞不能再拖累键盘响应(正是上一版反而恶化的原因)。
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    std::printf("=== mouse2controller ===\n");
    std::printf("侧键 + WASD -> 虚拟 Xbox360 左摇杆(设置窗口可实时调参,含旋转)\n");

    load_config();   // 加载 ini 配置(曲线/挡位/切挡键/触发键,失败回落出厂默认)
    std::printf("[触发键] %s(设置窗口可重新绑定)\n", to_utf8(trigger_name(g_trigger)).c_str());

    if (!g_pad.init()) {
        std::printf("\n[错误] 初始化虚拟手柄失败:%s\n", g_pad.error());
        std::printf("请确认 ViGEmBus 驱动已安装、ViGEmClient.dll 在同目录。\n");
        // 无控制台时 printf 看不见,弹窗提示(MessageBox 不依赖控制台,且替代原 _getch 等键)
        MessageBoxW(nullptr,
            L"初始化虚拟手柄失败。\n请确认 ViGEmBus 驱动已安装、ViGEmClient.dll 在同目录。",
            L"mouse2controller", MB_ICONERROR);
        return 1;
    }
    std::printf("[OK] 虚拟手柄已创建 (Dev #%u)\n", g_pad.index());

    // XInput 探测
    {
        HMODULE xlib = LoadLibraryA("xinput9_1_0.dll");
        if (!xlib) xlib = LoadLibraryA("xinput1_4.dll");
        if (!xlib) xlib = LoadLibraryA("xinput1_3.dll");
        if (xlib) {
            struct XG { unsigned short wButtons; unsigned char bLT, bRT; short lX, lY, rX, rY; };
            struct XS { unsigned long pkt; XG gp; };
            using Fn = unsigned long (WINAPI*)(unsigned long, XS*);
            auto gs = (Fn)GetProcAddress(xlib, "XInputGetState");
            if (gs) {
                bool any = false;
                for (unsigned long i = 0; i < 4; i++) {
                    XS st{};
                    if (gs(i, &st) == 0) { std::printf("[XInput] slot %lu 已连接\n", i); any = true; }
                }
                if (!any) std::printf("[XInput] 警告:XInput 未读到任何手柄\n");
            }
            FreeLibrary(xlib);
        }
    }

    // 启动输入工作线程(LL 钩子 + Raw Input + SetTimer + ViGEm 更新都在它里面,
    // 与主线程 ImGui 渲染隔离 → Present 阻塞不再影响键盘响应)
    DWORD inputTid = 0;
    HANDLE hInput = CreateThread(nullptr, 0, input_thread, nullptr, 0, &inputTid);

    std::printf("设置窗口已打开,拖滑块实时调参;控制台 t=测试模式 / q=退出。\n\n");

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    ui::create(hInst);

    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        if (_kbhit()) {
            int c = _getch();
            if (c == 'q' || c == 'Q' || c == 27) break;
            if (c == 't' || c == 'T') {
                g_testMode = !g_testMode;
                std::printf("[测试模式: %s]\n", g_testMode ? "开" : "关");
            }
        }
        if (ui::hwnd() && IsWindowVisible(ui::hwnd())) ui::render_frame();  // 仅可见时渲染
        Sleep(2);
    }

    // 通知输入线程退出并等它清理钩子/窗口(钩子卸载须在它的线程上下文)
    if (hInput) {
        PostThreadMessageW(inputTid, WM_QUIT, 0, 0);
        WaitForSingleObject(hInput, 2000);
        CloseHandle(hInput);
    }
    ui::shutdown();
    g_pad.release();
    g_mapper.reset();
    std::printf("已退出。\n");
    return 0;
}
