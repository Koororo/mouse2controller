// SPDX-License-Identifier: GPL-3.0
// Copyright (c) 2026 p2b
#pragma once
//
//  CurveMap.h — 推力-时间曲线(可编程按键→推力映射)
//  ------------------------------------------------------------
//  一条由控制点序列定义的曲线:横轴 = 按住时长(ms),纵轴 = 推力 [0,1]。
//  相邻点之间按「左点」的 interp 插值(Linear / Smooth / Step)。
//  自定义页的曲线编辑器可视化并编辑它;compute_target 用 eval() 取当前推力。
//
#include <vector>
#include <algorithm>
#include <cmath>

enum class Interp : int { Linear = 0, Smooth = 1, Step = 2 };

struct CurvePoint {
    float  t_ms;    // 时间(ms),>=0
    float  mag;     // 推力 [0,1]
    Interp interp;  // 到下一点的插值方式
};

struct CurveMap {
    std::vector<CurvePoint> pts;
    float Tmax = 2000.0f;
    int   selected = 0;

    // 取 t_ms 处的推力(逐点插值)
    float eval(float t_ms) const {
        if (pts.empty()) return 0.0f;
        if (t_ms <= pts.front().t_ms) return pts.front().mag;
        if (t_ms >= pts.back().t_ms)  return pts.back().mag;
        // upper_bound 找第一个 value<elem 的点 → comp(value, elem) 顺序
        auto it = std::upper_bound(pts.begin(), pts.end(), t_ms,
            [](float v, const CurvePoint& p){ return v < p.t_ms; });
        const CurvePoint& p0 = *(it - 1);
        const CurvePoint& p1 = *it;
        const float span = p1.t_ms - p0.t_ms;
        const float x = (span > 1e-6f) ? (t_ms - p0.t_ms) / span : 1.0f;
        switch (p0.interp) {
            case Interp::Step:   return p0.mag;                                   // 阶跃
            case Interp::Linear: return p0.mag + (p1.mag - p0.mag) * x;          // 直线
            case Interp::Smooth: {                                                // cosine ease(过两端、不过冲)
                const float e = (1.0f - std::cos(x * 3.14159265358979f)) * 0.5f;
                return p0.mag + (p1.mag - p0.mag) * e;
            }
        }
        return p0.mag;
    }

    void sortPts() {
        std::sort(pts.begin(), pts.end(),
                  [](const CurvePoint& a, const CurvePoint& b){ return a.t_ms < b.t_ms; });
    }

    void add(float t, float m) {
        pts.push_back({t, m, Interp::Smooth});
        sortPts();
        auto it = std::lower_bound(pts.begin(), pts.end(), t,
            [](const CurvePoint& p, float v){ return p.t_ms < v; });
        selected = (int)(it - pts.begin());
        if (selected >= (int)pts.size()) selected = (int)pts.size() - 1;
    }

    void removeSelected() {
        if (selected >= 0 && selected < (int)pts.size()) {
            pts.erase(pts.begin() + selected);
            if (selected >= (int)pts.size()) selected = (int)pts.size() - 1;
            if (pts.empty()) selected = -1;
        }
    }

    void setDefault() {
        pts.clear();
        pts.push_back({   0.0f, 0.00f, Interp::Smooth});
        pts.push_back({ 300.0f, 0.40f, Interp::Smooth});
        pts.push_back({1000.0f, 0.80f, Interp::Linear});
        pts.push_back({2000.0f, 1.00f, Interp::Linear});
        Tmax = 2000.0f;
        selected = 0;
    }
};
