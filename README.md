# mouse2controller

鼠标侧键 + 键盘 WASD → 虚拟 Xbox 360 手柄左摇杆。按住侧键,WASD 就是摇杆方向;松开回到普通键盘。

## 编译

需要 Visual Studio 2019/2022(带 C++ 桌面开发),Dear ImGui 源码放到 `third_party/imgui/`。

构建bat:

```
build.bat
```

或者 CMake:

```
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

构建产物在 `build/mouse2controller.exe`。运行还需要系统装好 [ViGEmBus 驱动](https://github.com/ViGEm/ViGEmBus/releases),以及 `ViGEmClient.dll` 跟 exe 放一起(`build_vigem.bat` 能从源码编出来)。

## 使用

1. 跑 `build/mouse2controller.exe`,看到「虚拟手柄已创建」就行。
2. 按住鼠标侧键,再按 WASD,左摇杆输出对应方向,可斜向组合。
3. 设置窗口里实时调推力、平滑、回中、旋转、挡位。
4. 通过滚轮切换不同挡位。
5. 曲线开启后,推力会根据曲线图点位变化而变化,滚轮不再参与调整档位。

## 致谢

- 过期薯条#5220
- [Dear ImGui](https://github.com/ocornut/imgui)(MIT)
- [ViGEmBus](https://github.com/nefarius/ViGEmBus)(BSD-3-Clause)
- [ViGEmClient](https://github.com/nefarius/ViGEmClient)(MIT)


---

GPL-3.0,详见 [LICENSE](LICENSE)。
