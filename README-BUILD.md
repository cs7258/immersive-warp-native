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

## 快捷键（全局，任何窗口有焦点都生效）

| 键 | 作用 |
|---|---|
| F3 | 进入 / 退出校准（几何校正模式） |
| F10 | 几何校正 ↔ 边缘融合模式切换 |
| Ctrl+F12 | 切换参与融合的边：ALL → 左 → 右 → 上 → 下 → NONE → ALL（随配置保存）；控制台窗口有焦点时直接按 F12 也可（F12 被 Windows 保留给调试器，无法注册为全局热键，所以全局绑定是 Ctrl+F12，另有同名按钮） |
| Ctrl+F3 | 截图校准：冻结当前捕获帧，在静止画面上对齐；再按恢复实时 |
| D | 打开 RGB 高级对话框（亮度/对比度/Gamma R G B），仅在校准状态注册 |
| Esc | 退出校准 |
| 方向键 / Shift+方向键 | 微调选中的控制点（1px / 10px） |
| 滚轮 | 融合模式下调该边 Gamma |

状态栏（控制台底部）实时显示：`Projector: N [i]` / 模式 / Snapshot / `Color balancing: R() G() B()` / 当前融合边。

## 单屏模式（无投影仪也能用）

- **图片源**："Load image..." 选择 PNG/JPG/BMP（系统 WIC 解码），勾选 "Image source" 后图片替代桌面捕获作为校正内容，几何/融合校准直接在真实图像上做。宽高比不同时自动信箱式适配（多余区域为黑）。
- **窗口化预览**：勾选 "Windowed preview (single screen)" 后输出变为 60% 大小的可拖动窗口，并通过 `WDA_EXCLUDEFROMCAPTURE` 将自身从桌面捕获中排除——因此单显示器也能实时扭曲桌面画面且不产生画面递归（捕获帧中该窗口区域为黑，属预期行为）。Win10 2004 之前系统无此 API 时自动禁用捕获，回退测试图/图片源。

## CMDNET TCP 控制服务（演出集成）

- 控制台勾选 "TCP control (CMDNET)" 或配置 `cmdnet=1` 后，监听 `127.0.0.1:5252`（仅本机回环，端口由 `cmdnetPort=` 配置）。
- 协议与原版 CMDNET 一致：ASCII 明文 `<COMMAND> <PARAM>;\n`，一请求一应答。命令不区分大小写。

| 命令 | 应答 | 说明 |
|---|---|---|
| `PING;` | `OK` | 连通性 |
| `STATUS;` | `calib=off;mode=warp;edges=ALL;snapshot=off;outputs=1` | 当前状态 |
| `CALIB ON;\|CALIB OFF;` | `OK calib=on` | 进入/退出校准（同 F3） |
| `MODE WARP;\|MODE BLEND;` | `OK mode=blend` | 校准模式切换（同 F10，未校准时自动进入校准） |
| `EDGES ALL;` / `EDGES L;` / `EDGES T+B;` | `OK edges=T+B` | 直接设置参与融合的边（ALL/L/R/T/B/NONE 及 `+` 组合） |
| `SNAPSHOT ON;\|SNAPSHOT OFF;` | `OK snapshot=on` | 冻结/恢复捕获帧（同 Ctrl+F3） |
| `SAVE;` | `OK saved` | 保存配置到 config.txt |
| 其他 | `ERR UNKNOWN` | 未知命令 |

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
