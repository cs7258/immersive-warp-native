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
3. "Source:" 选择捕获源桌面（默认 `Auto` = 第一个「不是输出」的显示器；多屏机器上可指定具体某个桌面作为画面来源，改动后输出窗口自动重建）。输出窗口标题带捕获源标记 `<label>   [src DISPLAY2]`，便于节目控制/测试工具按标题识别窗口
4. 多台投影仪做拼接墙时，用 "Layout:" 选 NxN 平铺（如 `2x2`），"Overlap:" 设相邻重叠带宽（0–40%）：程序按显示器物理位置把启用的输出排进网格，每台只显示源桌面对应窗口，内部边界自动设为融合边。注意这会重置各输出的 warp 网格，先 Layout 再校准
5. "Start Output" 启动全屏输出（先勾 Test pattern 显示测试图）
6. "Configure alignment" 或 F3 进入校准：
   - 拖绿色控制点做几何校正（方向键微调，Shift+方向键大步）
   - F10 切到融合模式：拖红点调融合宽度，滚轮调 gamma，Ctrl+滚轮调 gain，Shift+滚轮调 slope（融合曲线参数，默认 0/0 = 线性斜坡）
   - 勾选 "Advanced blending" 后为蓝点（每边多个）
   - Esc 退出校准
7. "Save Config" 持久化（%APPDATA%\ImmersiveWarpNative\config.txt），下次启动自动加载；"Preset:" 行可把当前整套配置（几何/融合/掩码/布局）存为命名预设并随时一键切回——预设文件与 CMDNET 的 `SAVEAS`/`LOADAS` 命令共用同一目录，名称规则同为 A-Z 0-9 `_` `-`；**托盘图标右键菜单也带 "Load preset" 子菜单**（逐项列出预设，单击即切换）
8. "Configure display..." 打开每投影机配置对话框（对应原版同名牌）：七标签页 **Bezels / Align / Geometry / Blending / Color / Masking / Help**
   - **Geometry**：控制点 Horiz/Vert 密度（3–24，Apply 会重置网格）、水平/垂直镜像、旋转 90°、Reset grid
   - **Blending**：按边（Left/Right/Top/Bottom）设置 Fade/Gamma/Gain/Slope，可勾选"Apply to all 4 edges"同步
   - **Color**：本投影机独立的 亮度/对比度/Gamma RGB 滑杆（与全局 Color adjustment 对话框区分，后者改所有启用输出）
   - **Masking**：黑位掩码矩形列表（最多 8 个，渲染为黑色）。选中列表项后在 X/Y/W/H 输入框按百分比编辑位置与尺寸，Apply 写入（自动限幅保证矩形不出屏），Reset 10% 复位为左上角 10%x10%；**也可直接在输出窗口上右键拖拽画掩码**（拖拽中实时黑块预览，松手落地，任何模式可用；误点 <1% 自动忽略）；结果随 Save Config 持久化为 `mask=x,y,w,h`（归一化 0..1）
   - Bezels / Align 为占位说明（未实现）；Help 是快捷键速查

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
| `STATUS;` | `calib=off;mode=warp;edges=ALL;snapshot=off;outputs=1;layout=1x1;overlap=0;curved=off;masks=0;testpattern=on;source=auto;out=auto` | 一次性状态摘要（后 7 项为扩展字段，追加在末尾，旧解析端按 `key=value;` 配对读取不受影响） |
| `CALIB ON;\|CALIB OFF;` | `OK calib=on` | 进入/退出校准（同 F3） |
| `MODE WARP;\|MODE BLEND;` | `OK mode=blend` | 校准模式切换（同 F10，未校准时自动进入校准） |
| `EDGES ALL;` / `EDGES L;` / `EDGES T+B;` | `OK edges=T+B` | 直接设置参与融合的边（ALL/L/R/T/B/NONE 及 `+` 组合） |
| `SNAPSHOT ON;\|SNAPSHOT OFF;` | `OK snapshot=on` | 冻结/恢复捕获帧（同 Ctrl+F3） |
| `SAVE;` | `OK saved` | 保存配置到 config.txt |
| `LOAD;` | `OK loaded default;outputs=1` | 从 config.txt 重新加载配置（等效 UI 的 Load 按钮；同时恢复预览/采样/测试图/图像源） |
| `SAVEAS name;` | `OK saved preset=NAME` | 把当前配置存为命名预设（`%APPDATA%\ImmersiveWarpNative\presets\NAME.cfg`；名称仅 A-Z 0-9 `_` `-`，最长 32，命令会整体转大写） |
| `LOADAS name;` | `OK loaded preset=NAME;outputs=1` | 加载命名预设（演出场景切换）；预设不存在报 ERR |
| `DELPRESET name;` | `OK deleted preset=NAME` | 删除命名预设；预设不存在或名称非法报 ERR |
| `GET presets;` | `OK presets=NAME1;NAME2` / `OK presets=none` | 列出全部已保存预设 |
| `SET left.fade=0.2;` / `SET top.gamma=1.3;` | `OK left.fade=0.2` | 远程调融合参数（fade 0–0.5、gamma 0.1–5，自动限幅并随配置生效） |
| `SET left.gain=0.8;` / `SET left.slope=0.3;` | `OK left.gain=0.8` | 融合曲线增益/陡度（gain 0–2、slope 0–1；0/0 = 线性斜坡，slope=1 为硬切换） |
| `SET testpattern=off;` | `OK testpattern=off` | 切换测试图 |
| `GET left.fade;` / `GET calib;` / `GET sampling;` 等 | `OK left.fade=0.150` | 查询单项状态（calib/mode/edges/snapshot/outputs/testpattern/sampling/autostart/source/out/各边 fade·gamma·gain·slope） |
| `GET config;` | `OK sampling=8;…;out0.dev=DISPLAY1;out0.rows=10;out0.left.fade=0.150;out0.masks=1 0.1,0.2,0.3,0.4;…;calib=off;out=auto` | **全量状态导出**：全局键 + 每输出一个 `outN.*` 块（网格密度、edges、四边融合参数、颜色调节、掩码列表——矩形间用空格分隔），与 STATUS 同为 `key=value;` 配对格式，一次往返镜像全部 UI 状态 |
| `GET displays;` | `OK displays=2;DISPLAY1=1920x1080*;DISPLAY2=1920x1080;` | 列出显示器拓扑（`*` = 主显示器），便于节目控制端挑捕获源 |
| `SET source=DISPLAY2;` / `SET source=auto;` | `OK source=DISPLAY2` | 指定捕获源桌面（auto = 第一个非输出的显示器）。改动会重建输出窗口；若指定的显示器本身是输出，引擎按防递归规则回退自动选择 |
| `GET out;` / `SET out=DISPLAY2;` / `SET out=auto;` | `OK out=DISPLAY2` | **per-output 定向**：设定后续 per-output 命令（masks、各边 fade/gamma/gain/slope、rows/cols、flip/rotate、edges 的 GET/SET）只作用于该输出（按显示器名或 label 匹配，名字须存在否则 ERR）。auto = 旧默认（掩码/融合写第一台启用输出，网格操作写全部） |
| `GET outputs;` | `OK outputs=2;DISPLAY1;DISPLAY2;` | 列出启用输出的短设备名（旧解析端只读首个数字不受影响） |
| `GET layout;` / `SET layout=2x2;` | `OK layout=2x2` | NxN 平铺布局：按物理位置（先上后下、先左后右）把启用的输出排进网格，第 (r,c) 格只显示源桌面对应窗口，出界部分被裁掉。会重建所有 warp 网格并把内部边界设为融合边 |
| `GET overlap;` / `SET overlap=15;` | `OK overlap=15` | 相邻投影仪的重叠带宽（单台投影画面的百分比，0–40）。改变时重建 warp 网格，与 layout 配合使用 |
| `GET engine;` / `SET engine=dxgi;` | `OK engine=desktopduplication` | 查询/选择 warp 引擎；当前构建只有 Desktop Duplication，其余为占位 |
| `GET rows;` / `GET cols;` | `OK rows=10` | 查询（第一个启用的）输出的控制点网格密度 |
| `SET rows=8;` / `SET cols=16;` | `OK rows=8` | 重建所有启用输出的控制点网格（3–24；**会丢弃手工校正点**，与对话框 Apply 一致） |
| `SET flip=H;` / `SET flip=V;` | `OK flip=H` | 所有启用输出水平/垂直镜像（对已校正的点阵做变换） |
| `SET rotate=90;` / `SET rotate=-90;` | `OK rotate=90` | 所有启用输出旋转 90°（顺/逆时针） |
| `GET curved;` / `SET curved=on;` | `OK curved=on` | 弯曲网格：控制点间 Catmull-Rom 插值（需 Horiz/Vert ≥ 4，否则自动回退双线性）。off = 双线性（默认） |
| `GET masks;` | `OK masks=2;0.100,0.200,0.300,0.400;…` | 列出命令目标输出（默认第一个启用输出）的黑位掩码矩形（归一化 0..1，x,y,w,h） |
| `SET mask.add=x,y,w,h;` / `SET mask.set=i,x,y,w,h;` | `OK masks=2` | 新增 / 按索引（0 基）改写掩码矩形；值域 0..1 越界报 ERR，w/h 自动限幅保证矩形不出屏；上限 8 个；下一帧生效并随 Save Config 持久化 |
| `SET mask.del=i;` / `SET mask.clear;` | `OK masks=1` / `OK masks=0` | 删除指定索引 / 清空全部掩码 |
| `SET mask.load=x,y,w,h x,y,w,h …;` | `OK masks=3` | 批量导入并整体替换掩码列表（矩形间用空格分隔——分号是协议终结符；校验与限幅规则同 mask.add，超过 8 个报 ERR） |
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
