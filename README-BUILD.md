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
3. "Source:" 选择捕获源桌面（默认 `Auto` = 第一个「不是输出」的显示器；多屏机器上可指定具体某个桌面作为画面来源，改动后输出窗口自动重建）
4. 多台投影仪做拼接墙时，用 "Layout:" 选 NxN 平铺（如 `2x2`），"Overlap:" 设相邻重叠带宽（0–40%）：程序按显示器物理位置把启用的输出排进网格，每台只显示源桌面对应窗口，内部边界自动设为融合边。注意这会重置各输出的 warp 网格，先 Layout 再校准
5. "Start Output" 启动全屏输出（先勾 Test pattern 显示测试图）
6. "Configure alignment" 或 F3 进入校准：
   - 拖绿色控制点做几何校正（方向键微调，Shift+方向键大步）
   - F10 切到融合模式：拖红点调融合宽度，滚轮调 gamma，Ctrl+滚轮调 gain，Shift+滚轮调 slope（融合曲线参数，默认 0/0 = 线性斜坡）
   - 勾选 "Advanced blending" 后为蓝点（每边多个）
   - Esc 退出校准
7. "Save Config" 持久化（%APPDATA%\ImmersiveWarpNative\config.txt），下次启动自动加载

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
| `SET left.fade=0.2;` / `SET top.gamma=1.3;` | `OK left.fade=0.2` | 远程调融合参数（fade 0–0.5、gamma 0.1–5，自动限幅并随配置生效） |
| `SET left.gain=0.8;` / `SET left.slope=0.3;` | `OK left.gain=0.8` | 融合曲线增益/陡度（gain 0–2、slope 0–1；0/0 = 线性斜坡，slope=1 为硬切换） |
| `SET testpattern=off;` | `OK testpattern=off` | 切换测试图 |
| `GET left.fade;` / `GET calib;` / `GET sampling;` 等 | `OK left.fade=0.150` | 查询单项状态（calib/mode/edges/snapshot/outputs/testpattern/sampling/autostart/source/各边 fade·gamma·gain·slope） |
| `GET displays;` | `OK displays=2;DISPLAY1=1920x1080*;DISPLAY2=1920x1080;` | 列出显示器拓扑（`*` = 主显示器），便于节目控制端挑捕获源 |
| `SET source=DISPLAY2;` / `SET source=auto;` | `OK source=DISPLAY2` | 指定捕获源桌面（auto = 第一个非输出的显示器）。改动会重建输出窗口；若指定的显示器本身是输出，引擎按防递归规则回退自动选择 |
| `GET layout;` / `SET layout=2x2;` | `OK layout=2x2` | NxN 平铺布局：按物理位置（先上后下、先左后右）把启用的输出排进网格，第 (r,c) 格只显示源桌面对应窗口，出界部分被裁掉。会重建所有 warp 网格并把内部边界设为融合边 |
| `GET overlap;` / `SET overlap=15;` | `OK overlap=15` | 相邻投影仪的重叠带宽（单台投影画面的百分比，0–40）。改变时重建 warp 网格，与 layout 配合使用 |
| `GET engine;` / `SET engine=dxgi;` | `OK engine=desktopduplication` | 查询/选择 warp 引擎；当前构建只有 Desktop Duplication，其余为占位 |
| `AUTOSTART ON;\|AUTOSTART OFF;` | `OK autostart=on` | 开机自启（写入 HKCU Run，无需管理员） |
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
