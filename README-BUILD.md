# Immersive Warp Native（C++ / Direct3D 11 生产版）

Immersive Display PRO 的原生复刻版，P0 范围：Windows 桌面捕获 → 网格几何校正 → 软边融合 → 多输出。

## 技术栈（选型结论）

- **C++17 + Direct3D 11 + DXGI Desktop Duplication**（与原版同路线）
- 零第三方依赖：无 Qt/ImGui/nlohmann，UI 用纯 Win32，配置用内置文本格式
- 目标体积：单 exe 预计 < 2MB，安装包 < 10MB（原版 38MB 还捆了 DirectX 运行时安装器）
- 延迟：桌面复制 1 帧，无注入、无驱动组件（规避杀软问题，参考 WSupportLdr 情报）

## 构建

1. 安装工具链（机器上当前没有 VS，管理员 PowerShell 执行一次）：

```
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

2. 双击或命令行运行 `build.bat` → 产出 `build\ImmersiveWarpNative.exe`

## 使用（同原版流程）

1. 投影仪扩展桌面接入 Windows
2. 控制台列表中选中显示器 → "Toggle selected as projector"
3. "Start Output" 启动全屏输出（先勾 Test pattern 显示测试图）
4. "Configure alignment" 或 F3 进入校准：
   - 拖绿色控制点做几何校正（方向键微调，Shift+方向键大步）
   - F10 切到融合模式：拖红点调融合宽度，滚轮调 gamma
   - 勾选 "Advanced blending" 后为蓝点（每边多个）
   - Esc 退出校准
5. "Save Config" 持久化（%APPDATA%\ImmersiveWarpNative\config.txt），下次启动自动加载

## 工程结构

| 文件 | 职责 |
|------|------|
| src/Common.h | 配置数据模型（点位/融合/色彩/遮罩）、全局状态 |
| src/Config.cpp | 显示器枚举、配置持久化 |
| src/Engine.h/.cpp | D3D11 设备、桌面复制（IDXGIOutputDuplication）、扭曲网格（Catmull-Rom 细分）、融合/色彩/遮罩像素着色器、校准覆盖层 |
| src/Windows.cpp | 输出窗口 + 校准交互（拖点/滚轮/F10/Esc）+ 控制台 UI |
| src/main.cpp | 启动、渲染线程、消息循环 |

## P0 边界与后续路线

- 本版：单 GPU（adapter 0）、无色彩均衡 UI（数据模型已含）、无 Sets 热切换、无状态栏文字（引擎覆盖层后续补）
- P1: 色彩均衡 UI、Sets、MQTT/触控面板
- P2: swapchain hook 低延迟管线（届时需 EV 代码签名 + 杀软白名单，对应原版 WSupportLdr.exe 的合规路径）、NVIDIA NVAPI Warp and Blend（Quadro/RTX 专业卡免费 SDK，无需商务授权）
- P3: 相机自动对准、AI 帧插值
