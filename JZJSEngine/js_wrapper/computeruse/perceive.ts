/// <reference path="../types/globals.d.ts" />
/// <reference path="../types/fetch.d.ts" />
// computeruse/perceive.ts —— **感知层**(VLM 后端)：一张截图 → 元素清单(type/text/框)
//
// 为什么放在 TS 层(而不是 C++ 侧接 OCR/检测):
//   引擎自带 `fetch` ⇒ 直接打 OpenAI 兼容的**多模态**接口就能"看懂"截图, 不用等底层后端;
//   打哪台服务由 `ExploreOptions.vlm` / `setVlmConfig()` 决定(本包**不内置地址**)。
//   以后换 OCR/YOLO/OmniParser 之类, 换的就是这一个文件 —— 上面的树模型与动作逻辑不动。
//
// 契约: 返回的 `rect` 一律**归一化 [0,1]**(相对整图左上角), 与 types.ts 的 Rect 一致;
//       换后端时这条契约不变, 所以上层(explore/树/判分)不用跟着改。
import type { Perception, PerceptionElement, Rect, VlmOptions } from './types';

// 本包的图里没有共享 types(types/index.d.ts 只 reference 自己的文件),
// 所以按 widget/events 包的同样约定就地声明引擎注入的 console。
declare var console: { log(...a: any[]): void; error(...a: any[]): void; warn(...a: any[]): void };

// ————————————————————————————————————————————————
// 配置
// ————————————————————————————————————————————————

let _default: VlmOptions | null = null;

/** 设默认 VLM 后端(调用方不想每次传 ExploreOptions.vlm 时用; null 清除)。 */
export function setVlmConfig(cfg: VlmOptions | null): void { _default = cfg; }
export function vlmConfig(): VlmOptions | null { return _default; }

function resolveVlm(cfg?: VlmOptions | null): VlmOptions {
    const c = cfg || _default;
    if (!c || !c.url)
        throw new Error('[CU] 没有 VLM 感知后端: 传 ExploreOptions.vlm 或先 setVlmConfig({url, model})');
    return c;
}

// ————————————————————————————————————————————————
// 提示词(坐标口径写死在这里: 归一化 [x,y,w,h])
// ————————————————————————————————————————————————

const TYPES = 'button / label / edit / checkbox / radio / combobox / slider / tab / menu / icon / image / panel / other';

function r3(v: number): string { return (Math.round(v * 1000) / 1000).toFixed(3); }

/** 提示词里带上真实图片尺寸与一个"算好的例子" —— 比空口说"归一化"有效得多。 */
export function buildPrompt(imgW: number, imgH: number): string {
    const ex = (px: number, py: number, pw: number, ph: number): string => '[' + [
        r3(px / imgW), r3(py / imgH), r3(pw / imgW), r3(ph / imgH),
    ].join(', ') + ']';
    return [
        '你是界面感知模块: 看这张截图, 列出里面所有可见控件。',
        '这张图就是**窗口客户区**(不含标题栏/边框), 坐标以它为准。',
        '只输出 JSON(不要解释、不要 markdown 围栏):',
        '{"elements":[{"type":"button","text":"确定","bbox":[x,y,w,h]}]}',
        '',
        'type 只能取: ' + TYPES,
        '  button=按钮 | label=静态文字 | edit=输入框 | checkbox=复选框 | radio=单选按钮',
        '  combobox=下拉框 | slider=滑块 | tab=选项卡 | menu=菜单项 | icon=只有图标的按钮 | panel=容器/分组',
        '',
        'bbox = [x, y, w, h], 四个数都是**归一化浮点数**(0~1, 保留 3 位小数), 相对整张图片左上角 —— 不是像素!',
        '图片尺寸 ' + imgW + 'x' + imgH + ' 像素。例如一个框像素坐标 (200, 100)、宽 300、高 50, 就写 ' + ex(200, 100, 300, 50),
        'bbox 要框住控件的**整个可见区域**(按钮含内边距、输入框含边框), 不要只框住文字。',
        'text 原样抄写控件上的可读文字(不要翻译、不要改写); 没有文字就写空串。',
        '窗口标题栏/边框上的系统按钮(最小化/最大化/关闭)、任务栏与桌面内容都不用列。',
        '只列你确实看到的控件; 看不清的不要猜。',
    ].join('\n');
}

// ————————————————————————————————————————————————
// 回包解析(模型输出不可靠 ⇒ 这里全部"容错 + 清洗")
// ————————————————————————————————————————————————

/** 从 dataURL 读 PNG 的 IHDR 拿尺寸(base64 前 32 字符 = 24 字节, 不依赖任何库)。 */
export function pngSize(dataUrl: string): [number, number] {
    try {
        const comma = String(dataUrl).indexOf(',');
        if (comma < 0) return [0, 0];
        const head = String(dataUrl).slice(comma + 1, comma + 33);
        if (head.length < 32) return [0, 0];
        const bytes: Uint8Array = (Uint8Array as any).fromBase64(head);
        if (bytes[0] !== 0x89 || bytes[1] !== 0x50) return [0, 0];     // PNG 魔数
        const be = (o: number): number => ((bytes[o] << 24) | (bytes[o + 1] << 16) | (bytes[o + 2] << 8) | bytes[o + 3]) >>> 0;
        return [be(16), be(20)];
    } catch (e) {
        return [0, 0];
    }
}

/** 抠出 JSON: 容忍 ```json 围栏与前后废话。 */
export function extractJson(text: string): any {
    const s = String(text || '');
    let body = s;
    const fence = s.indexOf('```');
    if (fence >= 0) {
        const nl = s.indexOf('\n', fence);
        const end = nl >= 0 ? s.indexOf('```', nl + 1) : -1;
        if (nl >= 0 && end > nl) body = s.slice(nl + 1, end);
    }
    const i = body.indexOf('{'), j = body.lastIndexOf('}');
    if (i < 0 || j <= i) throw new Error('[CU] VLM 回复里找不到 JSON: ' + s.slice(0, 200));
    return JSON.parse(body.slice(i, j + 1));
}

function num(v: any): number {
    const n = typeof v === 'number' ? v : parseFloat(String(v));
    return isFinite(n) ? n : NaN;
}

function clamp01(v: number): number { return v < 0 ? 0 : (v > 1 ? 1 : v); }

/** 模型常把**像素**与**归一化**混着给(实测出现过 [731, 0, 0.08, 0.12]) ⇒ 逐值看: >1 的按像素换算。 */
function toNorm(v: number, dim: number): number {
    if (!isFinite(v)) return NaN;
    if (v > 1.0001) return dim > 0 ? v / dim : NaN;
    return v;
}

/** bbox 清洗: 收数组/对象两种形状, 逐值归一化, 裁进 [0,1]; 无效返回 null。 */
export function fixRect(raw: any, imgW: number, imgH: number): Rect | null {
    let v: number[] | null = null;
    if (Array.isArray(raw) && raw.length >= 4) {
        v = [num(raw[0]), num(raw[1]), num(raw[2]), num(raw[3])];
    } else if (raw && typeof raw === 'object') {
        const pick = (...keys: string[]): any => { for (const k of keys) if (raw[k] !== undefined) return raw[k]; return undefined; };
        const x = pick('x', 'left'), y = pick('y', 'top'), w = pick('w', 'width'), h = pick('h', 'height');
        if (x !== undefined && w !== undefined) v = [num(x), num(y), num(w), num(h)];
    }
    if (!v) return null;

    let x = toNorm(v[0], imgW), y = toNorm(v[1], imgH);
    let w = toNorm(v[2], imgW), h = toNorm(v[3], imgH);
    if (!isFinite(x) || !isFinite(y) || !isFinite(w) || !isFinite(h)) {
        console.warn('[CU] bbox 无法归一化(图片尺寸未知?): ' + JSON.stringify(raw));
        return null;
    }
    x = clamp01(x); y = clamp01(y);
    if (x + w > 1) w = 1 - x;                      // 越界就裁到图内
    if (y + h > 1) h = 1 - y;
    if (w <= 0.002 || h <= 0.002) return null;     // 退化框丢掉
    return { x, y, w, h };
}

/** 模型给的"视觉类" → 本包词汇(与判分的 TYPE_FAMILY 对齐)。加词汇只改这张表。 */
const TYPE_MAP: Record<string, string> = {
    button: 'button', pushbutton: 'button', btn: 'button', icon: 'button', iconbutton: 'button',
    clickable: 'button', imagebutton: 'button',
    link: 'link', hyperlink: 'link',
    label: 'label', text: 'label', static: 'label', textblock: 'label', textlabel: 'label', caption: 'label',
    edit: 'edit', textbox: 'edit', textfield: 'edit', text_field: 'edit', input: 'edit', inputbox: 'edit',
    lineedit: 'edit', searchbox: 'edit', searchfield: 'edit', spinbox: 'edit', textarea: 'edit',
    checkbox: 'checkbox', check: 'checkbox', switch: 'checkbox', checkbutton: 'checkbox',
    radio: 'radio', radiobutton: 'radio', radio_button: 'radio',
    combobox: 'combobox', combo: 'combobox', dropdown: 'combobox', select: 'combobox', listbox: 'combobox',
    slider: 'slider', trackbar: 'slider',
    progressbar: 'progressbar', progress: 'progressbar',
    tab: 'tab', tabitem: 'tab', tabwidget: 'tab', tabbar: 'tab',
    menu: 'menu', menubar: 'menu', menuitem: 'menu', toolbar: 'toolbar',
    list: 'list', listwidget: 'list', listview: 'list',
    tree: 'tree', treewidget: 'tree', table: 'table', tablewidget: 'table',
    canvas: 'canvas', image: 'image', picture: 'image', icon2: 'image', scrollbar: 'scrollbar',
    window: 'window', dialog: 'window', frame: 'window',
    panel: 'group', group: 'group', groupbox: 'group', pane: 'group', container: 'group', card: 'group',
};

export function mapType(raw: string): string {
    const k = String(raw || '').toLowerCase().trim().replace(/[\s\-]+/g, '_');
    return TYPE_MAP[k] || TYPE_MAP[k.replace(/_/g, '')] || (k || 'unknown');
}

function asText(v: any): string {
    if (v === null || v === undefined) return '';
    return typeof v === 'string' ? v : String(v);
}

// ————————————————————————————————————————————————
// 感知主入口
// ————————————————————————————————————————————————

/**
 * 看一张图, 返回元素清单。
 * @param shot dataURL(`data:image/png;base64,...`)
 * @param opts.vlm 后端配置; 不传用 setVlmConfig 设的默认值
 */
export async function perceive(shot: string, opts?: { vlm?: VlmOptions }): Promise<Perception> {
    const cfg = resolveVlm(opts && opts.vlm);
    if (!shot) throw new Error('[CU] perceive: 没有截图(dataURL 为空)');

    const [imgW, imgH] = pngSize(shot);
    if (!imgW || !imgH) console.warn('[CU] perceive: 读不出图片尺寸, 像素坐标的框会被丢弃');

    const body: any = {
        model: cfg.model || 'default',
        messages: [{
            role: 'user',
            content: [
                { type: 'text', text: cfg.prompt || buildPrompt(imgW, imgH) },
                { type: 'image_url', image_url: { url: shot } },
            ],
        }],
        max_tokens: cfg.maxTokens || 4096,
        temperature: 0,
        // ⚠ 思考模型(如 qwen3.x)默认把推理也当输出: content 会是空的、token 全烧在 reasoning 上。
        //   这是 llama.cpp 的开关名; 换服务端用 extraBody 覆盖。
        chat_template_kwargs: { enable_thinking: false },
    };
    if (cfg.extraBody) {
        const extra = cfg.extraBody as Record<string, unknown>;
        for (const k in extra) body[k] = extra[k];
    }

    const headers: Record<string, string> = { 'Content-Type': 'application/json' };
    if (cfg.apiKey) headers['Authorization'] = 'Bearer ' + cfg.apiKey;

    const t0 = Date.now();
    const res = await fetch(cfg.url, {
        method: 'POST',
        headers,
        body: JSON.stringify(body),
        timeout: cfg.timeoutMs || 600000,          // 本机 27B 要几十秒, 给足
    });
    const text = await res.text();
    if (!res.ok) throw new Error('[CU] VLM 请求失败(HTTP ' + res.status + '): ' + text.slice(0, 500));

    let data: any = null;
    try { data = JSON.parse(text); } catch (e) {
        throw new Error('[CU] VLM 回包不是 JSON: ' + text.slice(0, 300));
    }
    const msg = (data && data.choices && data.choices[0] && data.choices[0].message) || {};
    const content = typeof msg.content === 'string' ? msg.content : '';
    if (!content.trim()) {
        throw new Error('[CU] VLM 没返回内容'
            + (msg.reasoning_content ? '(只给了 reasoning —— 用 extraBody 关掉思考)' : '(content 为空)'));
    }

    const doc = extractJson(content);
    const raw: any[] = Array.isArray(doc) ? doc : (Array.isArray(doc.elements) ? doc.elements : []);
    const elements: PerceptionElement[] = [];
    for (const it of raw) {
        if (!it || typeof it !== 'object') continue;
        const rect = fixRect(it.bbox !== undefined ? it.bbox : it.rect, imgW, imgH);
        if (!rect) continue;
        const rawType = asText(it.type || it.role || it.className || 'unknown');
        elements.push({ type: mapType(rawType), text: asText(it.text !== undefined ? it.text : it.label), rect, rawType });
    }
    const title = asText((doc && doc.window && doc.window.title) || (doc && doc.title) || '');

    console.log('[CU] VLM 感知: elements=' + elements.length + '/' + raw.length
        + ' img=' + imgW + 'x' + imgH + ' elapsed=' + (Date.now() - t0) + 'ms');
    return { title, elements, raw: content };
}
