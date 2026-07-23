// SPDX-License-Identifier: GPL-3.0
// Copyright (c) 2026 p2b
#pragma once

// ============================================================
//  手感配置 —— 只改这里就能调整体验,无需动逻辑
// ============================================================
namespace cfg {

// 出厂默认触发键(运行时被 mouse2controller.ini 覆盖,设置窗口"绑定…"可改):
//   0 = XBUTTON1(鼠标侧键"后退",通常是拇指靠上的那个)
//   1 = XBUTTON2(鼠标侧键"前进")
constexpr int TRIGGER_BUTTON = 0;

// 推力大小(占摇杆满偏的比例,范围 0~1)。0.05 = 5%。
//   注意:太小可能低于游戏自带的摇杆死区,导致游戏不响应,按需调大。
constexpr float MAGNITUDE = 0.30f;

// 放开为 [0,1](见 main.cpp 的 SliderFloat / [gears] 加载),此常量仅作参考,未被代码强制为下限。
constexpr float MIN_MAGNITUDE = 0.31f;

// 平滑系数:摇杆值向目标方向靠近的速度,范围 (0, 1]。
//   1.0 = 按键立即到位、无延迟(你要的"立刻推");越小越柔和。
constexpr float SMOOTHING = 1.00f;

// 回中速度:松开按键时摇杆归零的速度,范围 (0, 1]。
//   1.0 = 松开立即回中(你要的"立刻回中");越小回中越慢。
constexpr float RETURN_SPEED = 1.00f;

// 摇杆更新间隔(毫秒,8 ≈ 125Hz)。
constexpr unsigned int UPDATE_INTERVAL_MS = 8;

}  // namespace cfg
