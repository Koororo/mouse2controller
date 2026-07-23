// SPDX-License-Identifier: GPL-3.0
// Copyright (c) 2026 p2b
#pragma once
//
//  GearList.h — 推力挡位列表(默认页滚轮切换)
//  ------------------------------------------------------------
//  一组按用户排序的推力值(范围 [0,1])。默认页"添加当前推力为挡位"会追加一个值;
//  滚轮在挡位之间按列表顺序切换(上滚=前一项,下滚=后一项)。
//  挡位可删除、改值、拖动排序,持久化到 mouse2controller.ini 的 [gears] section。
//  纯数据结构(不依赖 ImGui / config.h),与 CurveMap.h 同风格。
//
#include <vector>
#include <algorithm>

struct GearList {
    std::vector<float> values;   // 每个挡位的推力值,范围 [0,1]
    int   selected = -1;         // UI 编辑焦点(独立于运行时激活挡位);-1 = 未选

    static constexpr int MAX = 100;   // 挡位上限

    bool empty() const { return values.empty(); }
    int  size()  const { return (int)values.size(); }

    // 追加挡位(达上限返回 false);clamp [0,1];选中刚加入的项
    bool add(float m) {
        if ((int)values.size() >= MAX) return false;
        if (m < 0.0f) m = 0.0f; if (m > 1.0f) m = 1.0f;
        values.push_back(m);
        selected = (int)values.size() - 1;
        return true;
    }

    // 改指定挡位的值(选中后编辑面板用)
    void set(int i, float m) {
        if (i < 0 || i >= (int)values.size()) return;
        if (m < 0.0f) m = 0.0f; if (m > 1.0f) m = 1.0f;
        values[i] = m;
    }

    // 删除指定索引,修正 selected
    void removeAt(int i) {
        if (i < 0 || i >= (int)values.size()) return;
        values.erase(values.begin() + i);
        if (values.empty()) selected = -1;
        else if (selected >= (int)values.size()) selected = (int)values.size() - 1;
    }

    // 邻位交换(拖动排序用)。非法则不动。
    void swap(int a, int b) {
        if (a < 0 || b < 0 || a >= (int)values.size() || b >= (int)values.size()) return;
        std::swap(values[a], values[b]);
    }
};
