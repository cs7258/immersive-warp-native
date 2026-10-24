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
2. 控制台 **Displays** 页列表中选中显示器 → "Toggle selected as projector"（控制台为多页结构，对齐原版：**Displays / Warp & Blend / Image & Test / Settings** 四页 + 底部常驻条 "Configure alignment" / "Enable Desktop warping" / 扭曲引擎下拉 / 状态区，深色主题）
3. "Source:" 选择捕获源桌面（默认 `Auto` = 第一个「不是输出」的显示器；多屏机器上可指定具体某个桌面作为画面来源，改动后输出窗口自动重建）。输出窗口标题带捕获源标记 `<label>   [src DISPLAY2]`，便于节目控制/测试工具按标题识别窗口
4. 多台投影仪做拼接墙时，用 "Layout:" 选 NxN 平铺（如 `2x2`），"Overlap:" 设相邻重叠带宽（0–40%）：程序按显示器物理位置把启用的输出排进网格，每台只显示源桌面对应窗口，内部边界自动设为融合边。**`1x1` = 单输出异型校正**：主屏/单屏原位输出，不设融合边（需要融合边可用 `SET edges=ALL` 手动开）。注意这会重置各输出的 warp 网格，先 Layout 再校准
5. "Start Output" 启动全屏输出（先勾 Test pattern 显示测试图）
6. "Configure alignment" 或 F3 进入校准：
   - 拖绿色控制点做几何校正（方向键微调，Shift+方向键大步）
   - F10 切到融合模式：拖红点调融合宽度，滚轮调 gamma，Ctrl+滚轮调 gain，Shift+滚轮调 slope（融合曲线参数，默认 0/0 = 线性斜坡）
   - 勾选 "Advanced blending" 后为蓝点（每边多个）
   - Esc 退出校准
7. "Save Config" 持久化（%APPDATA%\ImmersiveWarpNative\config.txt），下次启动自动加载；"Preset:" 行可把当前整套配置（几何/融合/掩码/布局）存为命名预设并随时一键切回——预设文件与 CMDNET 的 `SAVEAS`/`LOADAS` 命令共用同一目录，名称规则同为 A-Z 0-9 `_` `-`；**托盘图标右键菜单也带 "Load preset" 子菜单**（逐项列出预设，单击即切换）
8. "Configure display..." 打开每投影机配置对话框（对应原版同名牌）：七标签页 **Bezels / Align / Geometry / Blending / Color / Masking / Help**
   - **Geometry**：控制点 Horiz/Vert 密度（3–24，Apply 会重置网格）、水平/垂直镜像、旋转 90°、Reset grid
   - **Blending**：按边（Left/Right/Top/Bottom）设置 Fade/Gamma/Gain/Slope，可勾选"Apply to all 4 edges"同步；**Curve editor...** 打开融合曲线编辑器（对应原版 Advanced blend control）：绘制所选边响应曲线 w(t)=pow(blendCurve(t,gain,slope),gamma)，**曲线上按住拖动**可实时整形（反解 gain，松开即 Apply 写盘），Gain/Slope/Gamma + **R/G/B chan** 三通道伽马参数编辑 + Apply，通道伽马≠1 时曲线图叠加该通道的彩色响应曲线，Add to history / Restore history(0) 每边历史栈（最多 8 条，含通道值）；分通道数据与 CMDNET `SET left.chanr/chang/chanb` 共享同一存储
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
| Ctrl+F9 | 原位覆盖（in-place overlay）急停开关：穿透状态下输出窗口点不到，随时 Ctrl+F9 一键开/关（View 菜单与托盘菜单有同步勾选项） |
| F4 | 窗口扭曲开/关：启动或停止全部输出窗口（Enabled 复选框同步翻转） |
| Ctrl+F4 | 桌面扭曲开/关：同 Ctrl+F9 的原位覆盖切换（原版语义对齐） |
| Ctrl+Shift+1..8 | 加载第 N 个命名预设（按预设名 ASCII 序，与 Preset 下拉 / `SAVEAS`/`LOADAS` 同一套文件；无第 N 个预设时状态栏提示） |
| D | 打开 RGB 高级对话框（亮度/对比度/Gamma R G B），仅在校准状态注册 |
| Esc | 退出校准 |
| 方向键 / Shift+方向键 | 微调选中的控制点（1px / 10px） |
| 滚轮 | 融合模式下调该边 Gamma |

状态栏（控制台底部）实时显示（1 秒刷新）：`Projector: N [i]` / 模式 / Snapshot / Preview / In-place / Engine / `Warping: ACTIVE|idle`（输出窗口活性，对齐原版右上角 Desktop warping ENABLED/DISABLED 语义）/ `Blackout: ON`（仅黑场时显示，对齐原版右上角 Blackout 项）/ `Color balancing: R() G() B()` / 当前融合边 / 捕获源 / `Engine: 59.9 fps  frame 2.31 ms (cap 1.42 + draw 0.87)  lat 8.3 ms  uptime hh:mm:ss`（渲染健康、逐段帧耗时与捕获→上屏端到端延迟，与 `GET perf` 同源；lat 仅 DXGI 引擎测量，MAG 模式不显示）。

## 单屏模式（无投影仪也能用）

- **图片源**："Load image..." 选择 PNG/JPG/BMP（系统 WIC 解码），勾选 "Image source" 后图片替代桌面捕获作为校正内容，几何/融合校准直接在真实图像上做。宽高比不同时自动信箱式适配（多余区域为黑）。
- **窗口化预览**：勾选 "Windowed preview (single screen)" 后输出变为 60% 大小的可拖动窗口，并通过 `WDA_EXCLUDEFROMCAPTURE` 将自身从桌面捕获中排除——因此单显示器也能实时扭曲桌面画面且不产生画面递归（捕获帧中该窗口区域为黑，属预期行为）。Win10 2004 之前系统无此 API 时自动禁用捕获，回退测试图/图片源。
- **原位覆盖（in-place overlay）**：Displays 页勾选 "In-place overlay (fullscreen, click-through)"（或 CMDNET `INPLACE ON`）后，输出窗口无边框铺满整台显示器、鼠标点击直接穿透到下面的桌面、且从捕获中排除——扭曲结果视觉上就是桌面本身，这就是 **1x1 主屏异型校正** 的日常用法。控制台始终置顶可随时关掉；**按 F3 进入校准时穿透自动旁路**（控制点直接在覆盖层上拖，Esc 退出后恢复穿透），状态栏显示 `In-place: CALIB (click-through bypassed)`；Ctrl+F9 随时急停开关。

## 控制台 UI（复刻原版交互）

- **多页布局**：Tab 条（Displays / Warp & Blend / Image & Test / Settings）+ 底部常驻条（Configure alignment / Enable Desktop warping / Engine 选择器）+ 结构化状态块，与原版控制台同构；View 菜单可切页并与 Tab 勾选联动。
- **菜单栏**：File（Load/Save Config、Exit）、View（四页切换、Hide console to tray）、Warping（Configure alignment F3、Cycle blend edges Ctrl+F12、Snapshot freeze Ctrl+F3、Test pattern）、Help（About）。
- **工具栏**：Tab 条上方一排扁平快捷按钮（Load / Save / Test / In-place / Calibrate / Start / Stop），右端固定 **Exit**（与原版图标条同位），复用菜单与页面控件的命令 ID——任何入口触发的都是同一份状态机。
- **Displays 页输出示意图**：按显示器真实相对位置摆放的虚线框图——启用输出为橙色虚线框并标 `[PROJECTOR]`，未启用为灰色虚线框（对应原版的显示器示意区）。
- **全套深色主题**：控制台窗口 + 全部子控件（列表/下拉/按钮/Tab）应用 `DarkMode_Explorer` 主题（Win10 1809+），配合深色背景与橙色状态行，观感对齐原版。

## CMDNET TCP 控制服务（演出集成）

- 控制台勾选 "TCP control (CMDNET)" 或配置 `cmdnet=1` 后，监听 `127.0.0.1:5252`（仅本机回环，端口由 `cmdnetPort=` 配置）。
- 协议与原版 CMDNET 一致：ASCII 明文 `<COMMAND> <PARAM>;\n`，一请求一应答。命令不区分大小写。

## OSC over UDP 控制服务（回环）

- 配置 `osc=1`（或 CMDNET `SET osc=on;`）后监听 `127.0.0.1:8000`（仅本机回环，端口由 `oscPort=` / `SET oscport=1024..65535` 配置，改端口即重启监听）。
- **映射契约**：OSC 地址段（去掉 `/`）= CMDNET 命令词，OSC 参数（i/f/s/T/F 类型标签）转字符串后用空格拼接成 CMDNET 行执行；应答为**纯文本**回发到发包方（便于集成方调试与自动化测试）。
  - `/ping` → `PING` → 回 `OK`
  - `/get` + s"perf" → `GET PERF` → 回 `OK perf=...`
  - `/set` + s"engine=1" → `SET ENGINE=1`（注意 CMDNET 整行大写语义继承）
- 不支持 `#bundle`（忽略）与扩展类型标签（b/h/d 等，整包忽略）；`T`→`on`、`F`→`off`。
- 与 CMDNET 共享同一 dispatch，故命令全集与下表完全一致；安全姿态同为仅回环（开放网段为后续显式决策）。

| 命令 | 应答 | 说明 |
|---|---|---|
| `PING;` | `OK` | 连通性 |
| `STATUS;` | `calib=off;mode=warp;edges=ALL;snapshot=off;outputs=1;layout=1x1;overlap=0;curved=off;masks=0;testpattern=on;source=auto;out=auto;probe=off` | 一次性状态摘要（后 8 项为扩展字段，追加在末尾，旧解析端按 `key=value;` 配对读取不受影响） |
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
| `SET left.chanr=1.5;` / `SET left.chang=0.8;` / `SET left.chanb=1.2;` | `OK left.chanr=1.5` | **R/G/B 分通道融合伽马**：该边权重变为逐通道 `pow(curve(t), gamma×chanC)`（chan 0.1–5，默认 1/1/1 与旧版逐像素一致），用于修平铺重叠区的色彩平衡。仅 DXGI 引擎在 shader 中生效；MAG 引擎 overlay 单 alpha 无法逐通道衰减桌面内容，保持标量权重（平台限制）。随配置持久化（`left.chan=R,G,B`），`GET config` 的 `outN.*.chan=` 可读 |
| `GET perf;` | `OK perf=uptime=123;frames=4567;fps=59.9;cap=1.42;draw=0.87;frame=2.31;lat=8.32;outputs=1;running=1` | 运行健康诊断：运行秒数、累计帧数、滚动 1 秒平均帧率（空闲 0.0）、**逐段帧耗时 EMA（毫秒）**：cap=桌面捕获段（桌面完全静止时≈0，AcquireNextFrame(0) 立即超时零拷贝）、draw=网格+常量+Draw+Present 段、frame=整帧 renderAll（≥ cap+draw，差值为配置快照/自愈开销）、**lat=端到端延迟**（桌面帧时间戳 DXGI LastPresentTime(QPC)→Present 的 EMA，渲染管线半程；MAG 引擎恒 0 未测量；扫描输出与投影仪输入延迟叠加在其上）、启用输出数、是否运行中 |
| `PERFLOG ON;` / `PERFLOG OFF;` / `PERFLOG;` / `GET perflog;` / `SET perflog=1;` | `OK perflog=on` | **perf 历史采样**：开启后主窗口 1s 定时器每 10 s 向 debug.log 写一行 `perf: fps=… cap=… draw=… frame=… lat=… frames=…`（真机排查帧耗时随时间变化）；持久化到 config 键 `perflog=`，默认 off |
| `PROBE ON;` / `PROBE OFF;` / `PROBE;` / `GET probe;` / `SET probe=on;` | `OK probe=on` | **相机对位探针**：在每个 warp 目标点画白色十字标记（含中心块，约 24px），校准相机检测十字实际落点后经 `SET warp.load=` 回灌修正。独立于校准模式（calib 关着也显示）；无参数 PROBE 为切换。MAG 引擎下为均匀 rows×cols 点阵分层 overlay（无 warp 网格，网格密度跟随 `SET rows/cols`） |
| `INPLACE ON;` / `INPLACE OFF;` / `INPLACE;` / `GET inplace;` / `SET inplace=on;` | `OK inplace=on` | **原位覆盖（主屏异型校正输出）**：输出窗口无边框覆盖整台显示器 + `WS_EX_TRANSPARENT` 鼠标穿透 + `WDA_EXCLUDEFROMCAPTURE` 捕获排除——扭曲后的桌面看起来就是桌面本身（用户态复刻原版 DWM 桌面扭曲的视觉效果）。切换会重建输出窗口；校准时先关掉（穿透模式下无法拖控制点），控制台窗口置顶可随时操作；随配置持久化 |
| `BLACKOUT ON;` / `BLACKOUT OFF;` / `BLACKOUT;` / `GET blackout;` / `SET blackout=on;` | `OK blackout=on` | **演出黑场（一键全黑）**：所有启用输出立即渲染纯黑——DXGI 引擎跳过网格/overlay 只 Clear+Present（下一帧生效，无需重建）；MAG 引擎由常驻 mask overlay 画全屏黑（toggle 时立即唤醒 250ms 轮询）且 probe 标记同步隐去。View 菜单 "Blackout" 同一开关（勾选标记同步）。**刻意不持久化**：控制台重启后永远亮着，不会黑场吓到现场；GET config 不含该键、STATUS 含 |
| `SET testpattern=off;` | `OK testpattern=off` | 切换测试图 |
| `SET testanim=1;` (0..3) / `GET testanim;` / `SET testpause=on;` / `TESTPAUSE;` / `GET testpause;` | `OK testanim=2` / `OK testpause=on` | **动态测试图（原版 Images 区 Animation/Paused）**：在迷你测试图上叠加滚动橙带（#E8940A，3s 一循环），方向 `0`=关 `1`=横向 `2`=纵向 `3`=对角；`TESTPAUSE`（无参 toggle）冻结相位便于读数。方向随 config 持久化（`testAnim=`），暂停为会话态（重启恢复动画）。仅迷你测试图模式生效（加载图片/桌面捕获时无叠加）。**UI 入口**：Image 页 "Animation:" 下拉（Off/Horizontal/Vertical/Diagonal）+ "Pause" 勾选框，与 CMDNET 双向同步（`refreshAnimUI`，SET 在 CMDNET 锁区内调用故为无锁参数化设计）。**两引擎均实现**：DXGI=shader 内滚动橙带（renderAll 单写者推相位）；MAG=test overlay 60ms timer + 共享相位时钟 `g_magAnimPhase`（GetTickCount64 累加、dt clamp 0.25、暂停冻结；带几何 s=frac(dot(p,dir)*6+phase)<0.16 与 shader 完全同构，PolyPolygon 画斜带），多 overlay 锁步滚动（两引擎不同时运行，相位独立无碍）；关掉动画/暂停时 timer 借 `g_magAnimShown` 一次性清带 |
| `SET checker=on;` / `GET checker;` | `OK checker=on` | **Checker overlay 20x10（原版 Images 区对齐辅助）**：输出画面上叠加 20x10 屏幕空间棋盘（白格 35% 半透明）——相邻投影仪的棋盘格跨界应完全重合，快速发现拼接错位；融合带内棋盘被融合曲线衰减（曲线作用可见）。DXGI 在 shader 中实现（masks 之后、融合权重之前）；MAG 画在常驻 mask overlay 层（250ms hash 轮询 + toggle 即时唤醒）。随 config 持久化（`checker=`），STATUS/GET status 双导出；UI 入口：Image 页 "Checker 20x10" 勾选框（与 CMDNET 双向同步） |
| `SET subdivide=16;` / `GET subdivide;` | `OK subdivide=16` | **网格细分密度（每控制格细分段数，1..64，默认 8）**：warp 网格在相邻控制点之间按 `subdivide` 段线性细分——校正曲率大（弧形屏/超短焦边缘）时提高到 16/32 让网格贴住目标曲线（控制点只在格点上，细分段中点跟随两侧行/列插值）；平面屏 8 足够。改动立即触发整网重算（g_rev → buildMesh）；越界报 ERR、config 加载时 clamp 到 1..64（防手改配置顶点数爆炸）。随 config 持久化，STATUS/GET config 双导出 |
| `SET snap=1;` / `SET snap=C:\pics\out.png;` / `GET snap;` | `OK snap=requested` / `OK snap=<path>` | **帧快照导出 PNG**（命名避开已有的 `SNAPSHOT` 校准冻结命令）：**DXGI 引擎**在渲染线程 Present 前从交换链后台缓冲直接抓帧（**绕过 WDA_EXCLUDEFROMCAPTURE** 对 GetPixel/PrintWindow/BitBlt 的拦截）；**MAG 引擎**由 UI 线程对每个放大器 host 客户区做 GDI 抓屏（`SRCCOPY\|CAPTUREBLT`，含 mask/blend/probe 覆盖层合成结果），多输出时第 2/3 台加 `-2/-3` 后缀。两引擎均 WIC 编码 PNG。`snap=1` 存 `%APPDATA%\ImmersiveWarpNative\snapshots\snapshot-<时间戳>.png`（自动建目录）；`snap=<路径>` 自定义（**目录须已存在**；CMDNET 整行大写 → 路径以大写写入，Windows 大小写不敏感）。异步执行，`GET snap` 返回最近写入路径（请求时清为 none → 写入后更新，便于轮询 in-flight）；host 尚未建好时自动重试（有上限）。真机验证清单的画面留证手段 |
| `SET roi=12,8,12,8;` / `GET roi;` / `SET roicircle=on;` / `SET roiborder=4;` | `OK roi=12,8,12,8` | **Image area ROI（原版 Images 区 Top/Left/Right/Bottom px + Full circle + Border px）**：把测试图限制在感兴趣区内——L,T,R,B 四边 inset（输出像素，0-2048，缺省字段=0），图案在 ROI 内重新铺满 24x16；`roicircle=on` 只保留 ROI 内切圆（圆外黑，投影仪镜头为圆形时对位用）；`roiborder=<px>` 在 ROI 边缘内侧画绿色边框线（0-64，检查 ROI 边界位置）。仅测试图模式生效（桌面捕获/图片不受 ROI）。随 config 持久化（`roi=`/`roiCircle=`/`roiBorder=`），STATUS/GET status 双导出；UI 入口：Image 页 ROI 行（4 个 px 编辑框 + Circle 勾选 + Border 编辑框 + Apply / Sync L/R / Sync T/B / Reset，Sync 取两值平均后应用，与 CMDNET 经 WM_ROISYNC 双向同步） |
| `SET move=xy;` / `GET move;` | `OK move=xy` | **控制点移动模式（原版 Geometry 页 Move: Single / Move XY / All points）**：校准模式下拖拽或方向键微调 warp 控制点时的作用范围——`single`=只动抓取的点；`xy`=该点所在整行+整列一起平移（十字刚体，等距行/列对齐用）；`all`=全部点整体平移（整幅画面平移）。会话态不持久化（原版对话框编辑器状态同样不落盘），STATUS/GET status 双导出；UI 入口：校准模式中按 **M** 循环 Single→XY→All，状态栏即时提示当前模式。拖拽与方向键 nudge 共用 `moveWarpPoints`（行优先 idx 反推行列） |
| `GET sampling;` / `SET sampling=3;` | `OK sampling=3` | **图像采样质量**（Warp 页下拉第 4 档）：0=Trilinear、1/2=各向异性 4x/8x、3=**Bi-cubic（Catmull-Rom 16-tap）**——DXGI 着色器内多 tap 重采样（负瓣锐化，放大/网格斜投影时比双线性更锐），源纹理尺寸经 cbuffer 传入、图像/桌面源都支持；MAG 引擎由 DWM 合成、无 shader，该档无效果。0–3 越界报 ERR；随配置持久化，下拉/控制台/LOAD 三入口经 `refreshSampComboUI` 双向同步 |
| `GET maxfps;` / `SET maxfps=30;` | `OK maxfps=30` | **输出帧率上限**（Settings 页 Max FPS 下拉 Off/24/30/60/120）：0=不限制（默认），否则 DXGI 渲染循环按帧间隔节流（QPC 截止时刻调度，落后即重同步不补帧；最后 ~2ms 自旋保证精度）——现场省电/降噪/投影仪灯泡友好。范围 0–240 越界报 ERR；随 config 持久化（`maxfps=`，加载 clamp），STATUS/GET config 双导出；UI 入口：Settings 页 "Max FPS:" 下拉（与 CMDNET 双向同步，`refreshMaxFpsUI`）。MAG 引擎无渲染循环、由 DWM 按需合成，该设置对其无效。静止桌面下 fps 本来就低（零拷贝超时），限帧断言需 `SET testanim=1` 强制帧流 |
| `GET color;` / `SET color=1.2,1.0,0.8,1.5,0.9;` | `OK color=1.200,1.000,0.800,1.500,0.900` | 命令目标输出的色彩调节：gammaR,gammaG,gammaB（0.2–3.0）+ bright,contrast（0.2–2.0），越界自动限幅到滑杆范围；遵循 `SET out=` 定向 |
| `GET left.fade;` / `GET calib;` / `GET sampling;` 等 | `OK left.fade=0.150` | 查询单项状态（calib/mode/edges/snapshot/outputs/testpattern/sampling/autostart/source/out/各边 fade·gamma·gain·slope·chanr/chang/chanb） |
| `GET config;` | `OK sampling=8;…;out0.dev=DISPLAY1;out0.rows=10;out0.left.fade=0.150;out0.masks=1 0.1,0.2,0.3,0.4;…;calib=off;out=auto` | **全量状态导出**：全局键 + 每输出一个 `outN.*` 块（网格密度、edges、四边融合参数、颜色调节、掩码列表——矩形间用空格分隔），与 STATUS 同为 `key=value;` 配对格式，一次往返镜像全部 UI 状态 |
| `GET displays;` | `OK displays=2;DISPLAY1=1920x1080*;DISPLAY2=1920x1080;` | 列出显示器拓扑（`*` = 主显示器），便于节目控制端挑捕获源 |
| `SET source=DISPLAY2;` / `SET source=auto;` | `OK source=DISPLAY2` | 指定捕获源桌面（auto = 第一个非输出的显示器）。改动会重建输出窗口；若指定的显示器本身是输出，引擎按防递归规则回退自动选择 |
| `SET out=DISPLAY1;` + `SET src=DISPLAY2;` / `SET src=auto;` / `GET srcs;` | `OK src=DISPLAY2 on 1 output(s)` / `OK srcs=1;src1=DISPLAY1>DISPLAY2` | **每输出口独立捕获源覆盖**（多屏异源：不同屏显示不同桌面/程序）：先 `SET out=` 定向，再 `SET src=<显示器名>`；无 target 时对所有启用输出生效；`auto`/`clear` 清除回退全局源。配置每输出持久化（`src=` 行）；覆盖指向启用输出屏时防递归守卫拒绝（回退全局源）；改动异步重建输出窗口（duplication 绑定在 attach 时）。`-` 表示跟随全局源。UI 入口：Displays 页选中输出行后的 "Override source:" 下拉（与 CMDNET 双向同步，`WM_OVRSYNC`） |
| `GET out;` / `SET out=DISPLAY2;` / `SET out=auto;` | `OK out=DISPLAY2` | **per-output 定向**：设定后续 per-output 命令（masks、各边 fade/gamma/gain/slope、rows/cols、flip/rotate、edges 的 GET/SET）只作用于该输出（按显示器名或 label 匹配，名字须存在否则 ERR）。auto = 旧默认（掩码/融合写第一台启用输出，网格操作写全部）。与控制台 "Target:" 下拉共享同一存储，任一入口修改另一侧实时同步 |
| `GET outputs;` | `OK outputs=2;DISPLAY1;DISPLAY2;` | 列出启用输出的短设备名（旧解析端只读首个数字不受影响） |
| `GET layout;` / `SET layout=2x2;` | `OK layout=2x2` | NxN 平铺布局：按物理位置（先上后下、先左后右）把启用的输出排进网格，第 (r,c) 格只显示源桌面对应窗口，出界部分被裁掉。会重建所有 warp 网格并把内部边界设为融合边；**内部边的融合宽度自动 = 重叠带宽**（相邻 tile 渐变互补，线性曲线下权重和恒为 1），外侧边保留手工值 |
| `GET overlap;` / `SET overlap=15;` | `OK overlap=15` | 相邻投影仪的重叠带宽（单台投影画面的百分比，0–40）。改变时重建 warp 网格并同步内部融合宽度，与 layout 配合使用 |
| `GET engine;` / `SET engine=DXGI;` / `SET engine=MAG;` | `OK engine=desktopduplication` / `OK engine=mag` | 查询/选择 warp 引擎。**DXGI** = Desktop Duplication + 网格校正/融合管线；**MAG** = DWM Magnification host 窗口（DWM 合成器直供桌面内容，无捕获会话/无渲染线程；自身窗口经 filter list 从镜像内容中排除防无限递归）。**MAG 支持 NxN 平铺**：每台 host 的源窗口按 layout/overlap 切割（与 DXGI 同几何语义），layout 改动自动重建 host；MAG 引擎下校准（F3）不可用并提示。**MAG 的测试图**为 GDI overlay（棋盘/网格线/红框/绿中心线，与 DXGI 着色器同款样式），同样走 `SET testpattern=` / 勾选框，overlay 本身被排除出镜像内容。**MAG 的软边融合**为 per-host 分层 overlay（alpha 渐变黑条），`EDGES` 掩码与 `SET left.fade` 同语义生效，**融合曲线（gain/slope/gamma）与 DXGI 着色器完全同款**（CPU 侧同公式计算权重，各边相乘），**Advanced blending 高级边（每边多点 pos/fade/gamma 插值）同样生效**（"Advanced blending" 勾选实时重算）；MAG 平铺下与 DXGI 共用「内部边 fade 自动 = 重叠带宽」的推导 |
| `GET rows;` / `GET cols;` | `OK rows=10` | 查询（第一个启用的）输出的控制点网格密度 |
| `GET dispchange;` / `SET rebuild=1;` | `OK dispchange=0` / `OK rebuild` | 显示器变化自适应：分辨率改动/热插拔触发 `WM_DISPLAYCHANGE` 后自动延迟重建输出窗口（DXGI duplication 本就会失效、MAG host 矩形已过期），计数器可查；`SET rebuild=1` 手动走同一重建路径（现场拔插扩展坞后也可用） |
| `SET rows=8;` / `SET cols=16;` | `OK rows=8` | 重建所有启用输出的控制点网格（3–24；**会丢弃手工校正点**，与对话框 Apply 一致） |
| `GET warp;` / `GET warp=r,c;` / `SET point=r,c,x,y;` | `OK point=1,1,0.1500,0.1200` | 远程校正单个控制点：`GET warp` 全量导出（`rows;cols;` + 每点 `x,y` 空格分隔，行优先），`GET warp=r,c` / `SET point=r,c,x,y` 读写单点（0 基，归一化坐标，允许 -0.5–1.5 梯形出界）；遵循 `SET out=` 定向 |
| `SET warp.load=x,y x,y …;` | `OK warp.load=120` | 批量导入并整体替换控制点网格（点数必须等于目标输出 rows×cols，坐标同上允许 -0.5–1.5 限幅）；与 `GET warp` 配对做网格快照/恢复与相机反馈自动对位 |
| `SET flip=H;` / `SET flip=V;` | `OK flip=H` | 所有启用输出水平/垂直镜像（对已校正的点阵做变换） |
| `SET rotate=90;` / `SET rotate=-90;` | `OK rotate=90` | 所有启用输出旋转 90°（顺/逆时针） |
| `GET curved;` / `SET curved=on;` | `OK curved=on` | 弯曲网格：控制点间 Catmull-Rom 插值（需 Horiz/Vert ≥ 4，否则自动回退双线性）。off = 双线性（默认） |
| `GET masks;` | `OK masks=2;0.100,0.200,0.300,0.400;…` | 列出命令目标输出（默认第一个启用输出）的黑位掩码矩形（归一化 0..1，x,y,w,h） |
| `SET mask.add=x,y,w,h;` / `SET mask.set=i,x,y,w,h;` | `OK masks=2` | 新增 / 按索引（0 基）改写掩码矩形；值域 0..1 越界报 ERR，w/h 自动限幅保证矩形不出屏；上限 8 个；下一帧生效并随 Save Config 持久化。**MAG 引擎同语义生效**：per-host 分层 overlay 画黑色矩形，内容由 250 ms 轮询自动跟上（无需显式重同步） |
| `SET mask.del=i;` / `SET mask.clear;` | `OK masks=1` / `OK masks=0` | 删除指定索引 / 清空全部掩码 |
| `SET mask.load=x,y,w,h x,y,w,h …;` | `OK masks=3` | 批量导入并整体替换掩码列表（矩形间用空格分隔——分号是协议终结符；校验与限幅规则同 mask.add，超过 8 个报 ERR） |
| `AUTOSTART ON;\|AUTOSTART OFF;` | `OK autostart=on` | 开机自启（写入 HKCU Run，无需管理员） |
| `INJECT LAUNCH <exe> [args];` | `OK INJECT launch pid=N` | 挂起启动目标程序并注入 WarpHook.dll（补丁先于任何 swapchain 创建）；exe 路径可加引号支持空格；DLL 默认取主程序同目录 WarpHook.dll |
| `INJECT PID <pid> [dll];` | `OK INJECT pid=N` | 注入运行中的进程（只捕获注入后创建的 swapchain）；拒绝注入引擎自身（防双重 hook）；坏 pid/缺 DLL 均 ERR 不伤主程序 |
| `LOADFILE <path>;` | `OK loaded file;outputs=N` | 全路径配置加载（MQTT loadprocalib 映射） |
| 其他 | `ERR UNKNOWN` | 未知命令 |

## 工程结构

| 文件 | 职责 |
|------|------|
| src/Common.h | 配置数据模型（点位/融合/色彩/遮罩）、全局状态 |
| src/Config.cpp | 显示器枚举、配置持久化 |
| src/Engine.h/.cpp | D3D11 设备、桌面复制（IDXGIOutputDuplication）、扭曲网格（Catmull-Rom 细分）、融合/色彩/遮罩像素着色器、校准覆盖层 |
| src/Windows.cpp | 输出窗口 + 校准交互（拖点/滚轮/F10/Esc）+ 控制台 UI |
| src/main.cpp | 启动、渲染线程、消息循环 |
| src/HookIPC.cpp | scanout 管线宿主侧：发布 WarpHook.dll 参数映射 |
| src/Mqtt.cpp | MQTT broker 客户端：`<prefix>/command` JSON 命令 + `<prefix>/status` retain 上报 |
| src/Inject.cpp | 内置注入集成：CMDNET `INJECT LAUNCH/PID` 复用 IWNInject 工作流（DLL 默认同目录 WarpHook.dll，拒绝自注入） |
| src/hook/WarpHook.cpp | 注入 DLL：Present hook + 就地扭曲（scanout 前） |
| src/hook/Injector.cpp | IWNInject.exe：挂起启动注入器 |

## P0 边界与后续路线

- 本版：单 GPU（adapter 0）、无状态栏文字（引擎覆盖层后续补）。Sets 热切换已补全：预设（Settings 页 Preset 下拉 / 托盘 / Ctrl+Shift+1..8 / CMDNET SAVEAS·LOADAS）加载后若启用输出集变化，输出窗口经 WM_DISPCHANGE 实时重建，无需重启
- **MQTT 对接已完成**（原版 v7.1.2 parity，src/Mqtt.cpp 零依赖 MQTT 3.1.1 客户端）：订阅 `<prefix>/command`（默认 `display/command`，prefix 经 SET mqttprefix 小写归一），JSON 命令映射——setconfigset→预设按名序 1..8、setdesktopwarping→输出引擎绝对开关、blackout→SET blackout、loadprocalib→LOADFILE 全路径加载、quit→退出；`<prefix>/status` retain 状态上报（连接时+每命令后）。QoS0 无鉴权（与 CMDNET 同信任模型）；SET mqtt/mqtthost/mqttport/mqttprefix 四键管理，断线 3s 重连
- **P1 全部清零**：色彩均衡 UI、scanout 注入管线（IPC v3 多槽位+标定覆盖层+Sets 热切换）、MQTT 对接。剩余：真机双屏验证（VERIFY-CHECKLIST.md A-H）、P2 内核驱动（暂缓——无对应硬件，届时需 EV 代码签名 + 杀软白名单，对应原版 WSupportLdr.exe 的合规路径）
- P3: 相机自动对准、AI 帧插值

### scanout 管线（Phase 1 已落地：端到端 hook 通道）

- **IWNInject.exe --launch <exe> <dll>**：挂起启动目标 → 注入 WarpHook.dll → 恢复运行（早注入，能拦到目标的全部 swapchain）；`--pid <pid> <dll>` 模式只对注入后新建的 swapchain 生效
- **WarpHook.dll**：进程内 patch DXGI 工厂 vtable（CreateSwapChain / CreateSwapChainForHwnd），对每个新 swapchain patch Present（槽 13，按实现类建表——位块/翻转模型 vtable 不同）；Present 前把后台缓冲 CopyResource→SRV，经四角 NDC 偏移的双线性网格自绘回 RTV（就地扭曲，原版同级的"帧在 scanout 前被改造"）
- **参数通道**：宿主发布命名映射 `IWN_WarpHook_v1`（`hook/hook_ipc.h` 契约），CMDNET 实时翻转
- **命令**：`GET hook` / `SET hook=on|off`（使能位）、`SET hooktest=on|off`（固定梯形四角偏移，肉眼可见验证）；STATUS / GET config 双导出 `hook=`
- **日志**：注入 DLL 写 `%APPDATA%\ImmersiveWarpNative\hook.log`（attach / vtable patch / swapchain hooked / warp pass frames）
- Phase 1 限制：扭曲模型为 4 角 NDC 偏移的 Phase-1 PoC（宿主全控制点网格导出、融合带着色器、per-swapchain 输出选择在后续 Phase）；目标应用需在 hook 安装后创建 swapchain（用 --launch）
