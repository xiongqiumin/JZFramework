---
name: TS computer-use
description: 看画面脚本必读。
---

1. 坐标只一套：截图/match/click 全为**相对整桌面归一化 0~1**，可互换。
2. `match(JSON串)` 传 `{kind:'color',color:'#rrggbb',region?}`，**只回最大 4-邻接连通同色块**，无→`null`；**容差 60**；枚举逐 `region` 问。
3. `click(x,y,'left')` 三参必给、发即返回⇒后接 `wait` 复核；`key` 用 Win32 键名。
4. `screenshot()`=桌面合成帧；`layoutTree` 看不到自绘画面；**容器里 OCR 不可用**。
5. 校准用大色块/空盘网格线；取样躲开格子正中（偏 (+8,−8)px）。
6. 整帧结构化：`computeruse` 包 `perceive(dataUrl)`（端点自配），出参仍要逐点复核。
7. 日志用 ASCII、路径用 `/`、`Buffer` 要 import；细节见源码。
