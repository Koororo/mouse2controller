// SPDX-License-Identifier: GPL-3.0
// Copyright (c) 2026 p2b
#pragma once
//
//  StickMapper.h — 目标方向 → 左摇杆(带平滑)
//  ------------------------------------------------------------
//  输入工作线程(WM_TIMER)根据按键状态算出目标摇杆方向(归一化 -1..1,
//  屏幕坐标:y 向下为正),调用 set_target;tick() 做一阶低通平滑后输出摇杆轴值。
//
#include <cmath>
#include "config.h"

// 运行时可调参数(定义在 main.cpp,由设置窗口修改)
extern float g_smooth;
extern float g_return;

class StickMapper {
   public:
    // 设置目标方向(归一化 -1..1,屏幕坐标)
    void set_target(float nx, float ny) {
        m_tx = nx;
        m_ty = ny;
    }

    // 推进一步平滑插值,输出左摇杆 (lx, ly),范围 [-32767, 32767]。
    void tick(short& outLX, short& outLY, float forceSpeed = -1.0f) {
        const bool zeroTarget = (m_tx == 0.0f && m_ty == 0.0f);
        // forceSpeed>=0 时强制平滑系数(st_mode 点按脉冲用 1.0 直冲,避免被 g_smooth 抹平);
        // 否则按 UI 的 g_smooth / g_return。
        const float speed = (forceSpeed >= 0.0f) ? forceSpeed
                          : (zeroTarget ? ::g_return : ::g_smooth);

        m_cx += (m_tx - m_cx) * speed;
        m_cy += (m_ty - m_cy) * speed;

        // 目标为零且已接近零时彻底归零,避免残余抖动
        if (zeroTarget && std::fabs(m_cx) < 0.0005f && std::fabs(m_cy) < 0.0005f) {
            m_cx = m_cy = 0.0f;
        }

        // 屏幕坐标 → 手柄坐标:y 轴取反(向上推为正)
        outLX = clamp(m_cx);
        outLY = clamp(-m_cy);
    }

    void reset() {
        m_cx = m_cy = 0.0f;
        m_tx = m_ty = 0.0f;
    }

   private:
    static short clamp(float v) {
        if (v >= 1.0f)  return 32767;
        if (v <= -1.0f) return -32767;
        return static_cast<short>(v * 32767.0f);
    }

    float m_cx{}, m_cy{};  // 当前输出(归一化)
    float m_tx{}, m_ty{};  // 目标(归一化)
};
