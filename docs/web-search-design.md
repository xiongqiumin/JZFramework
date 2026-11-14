# 网络搜索（web_search）现状与选型

> 2026-11-12 调研。结论先行：**必应 RSS 国内直连可用**（~6KB、10 条、链接直链），零依赖、无需
> API Key，已在 `web_search` 里作为唯一默认源。关键前提：带 `setlang=zh-CN&cc=CN`、跟随 302。
> 本文记录现状、实测数据、可选方案与接入方式。
>
> ⚠ 2026-11-13 追加：必应会按 **TLS 握手指纹**把疑似 bot 的请求判为爬虫，命中后**照常返回 200、
> RSS 结构完整，但内容是低质量的"影子结果"**（搜「美团外卖 客服电话」返回「美（汉语文字）」）。
> 换 UA / 带 cookie / 改参数都无效；只有 `item < 5` 能识别重度降级（`size` 不能用，正常与降级重叠）。
> 详见 §2.1。

---

## 1. 现状

### 1.1 代码位置

| 组件 | 位置 |
|---|---|
| 工具实现 | `binary/agent_tool/JZCodeTools.cpp` → `JZWebSearchTool` / `JZWebFetchTool` |
| 默认搜索源 | `kBingRssSearchUrl`（`format=rss&count=15&setlang=zh-CN&cc=CN`），单一源 |
| 结果解析 | `parseSearch()`（JSON / RSS 两条分支）→ `collectJsonResults()` / `collectRssResults()` |
| 失败判定 | 只认**硬错误**：连接失败 / 非 2xx / 解析不出条目 → 报错。**不做查询词相关性启发式**（误判两头挨骂，见 §7） |
| 工具注册 | `binary/agent_cli/JZAgentCliTask.cpp`（`allowNetworkTools` 为真时注册） |
| 配置项 | `network.enabled` / `network.searchUrl` / `network.proxyHost...` |
| 档位联动 | `binary/agent_cli/main.cpp`：未显式配置时 `allowNetworkTools = (profile != Strict)` |

### 1.2 执行流程

```
模型调用 web_search(query, num_results)
  → 把 {q} 换成原始查询文本（⚠ 不预编码，交给 JZUrl 统一编码一次）
  → JZNetworkAccessManager::get(url)      ← ⚠ 只支持 GET
      · UA = kSearchUserAgent（伪装 Chrome）+ Accept / Accept-Language / Sec-Fetch-*
      · 允许跨主机重定向（www.bing.com → cn.bing.com 的 302）
      · 单源超时 15s
  → parseSearch(body)
      · 以 { 或 [ 开头 → JSON：顶层数组 / results / RelatedTopics 三种形状
      · 含 <rss> 或 <feed> → RSS：<item><title>/<link>/<description>
  → formatSearchResults()：拼成"1. 标题\n  链接\n  摘要"
```

### 1.3 解析器认的字段

`collectJsonResults()` 对每个条目按下列候选 key 取值（大小写都认）：

| 语义 | 可用 key |
|---|---|
| 标题 | `title` / `Title` / `heading` / `result` |
| 链接 | `url` / `URL` / `href` / `link` / `FirstURL` |
| 摘要 | `content` / `snippet` / `description` / `body` / `Text` |

只要自定义服务返回 `{"results":[{...}]}` 就能直接对接，**客户端零改动**。

### 1.4 现有能力缺口

- ❌ **只发 GET**：DeepSeek / Tavily / 博查都是 POST；
- ❌ **无缓存**：同一 query 重复搜索每次都会重新请求（模型换词重搜很常见）；
- ❌ **无次数上限**：一个任务里可以无限次搜索；
- ❌ **无相关性校验**：源返回什么就喂什么（这次就是被垃圾结果坑的）；
- ❌ **无验证页识别**：抓取型源被限流时返回 200 + 三五 KB 的验证页，会被当成"无结果"。

---

## 2. 各搜索源实测（2026-11-12，本机）

| 源 | 结果 | 说明 |
|---|---|---|
| **Bing RSS**（默认源） | ✅ 可用 | 直连 + 跟随 302 到 `cn.bing.com` + `setlang/cc`：`冬瓜` / `python asyncio` / `上海天气` 三次实测均 10 条全相关，体积仅 ~6KB |
| **Bing HTML** | 已移除 | 曾作兜底（~105KB，10 条结果与 RSS 完全一致）；既然 RSS 一直可用，多一条解析路径只多一处会坏的地方，2026-11-12 删除（含 `jzParseBingHtmlResults` 与其单测） |
| **百度** | ❌ 不适合自动化 | 无 cookie 直接 302；先访问首页拿 `BAIDUID` + Referer 后第一次 200，**第二次就 302 到 `wappass.baidu.com/...tuxing_v2`（图形验证码）**，且粘性 |
| **搜狗** | ⚠️ 单次可用，限流严 | 浏览器 UA 下第 1–4 次（间隔 300ms）都是 200 / ~420KB / 1.3–1.5s；**第 5 次起**返回 200 + 3.5KB `antispider` 页，随后 302 到 `www.sogou.com/captcha/`；无 UA 时 200 但只有 5.4KB 提示页 |
| **DuckDuckGo API** | ❌ 不可达 | `code=000`，`/v1` 免费接口质量一般 |
| **DeepSeek Search API** | ✅ 推荐 | 见 §3，¥5 / 千次 |

实测原始数据（搜狗连发）：

```
#1 code=200 len=420005 ms=1454
#2 code=200 len=422192 ms=1368
#3 code=200 len=422517 ms=1395
#4 code=200 len=419855 ms=1279
#5 code=200 len=3486   ms=188   ANTISPIDER
#6 code=200 len=3486   ms=227   ANTISPIDER
```

### 2.1 已确认：必应会返回「影子 SERP」（2026-11-13 实测）

**现象**：`format=rss` 请求 HTTP 200、RSS 结构完整，但内容是
`美（汉语文字）_百度百科` / `饿（汉语文字）_百度百科` 这类**按首字命中的词典条目** ——
搜「美团外卖 客服电话」返回「美」，搜「饿了么 客服电话」返回「饿」。

**关键证据：同一串参数，20 分钟内两种结果**

| 时间 | 请求 | size | 首条结果 |
|---|---|---|---|
| 10:25 | `www.bing.com/search?q=美团外卖+客服电话&format=rss&count=15&setlang=zh-CN&cc=CN` | 3494 | 美（汉语文字）_百度百科 ❌ |
| 10:45 | **完全相同** | 4367 | 服务热线 - 美团 ✅ |

**逐条排除**（都有实测）：

| 假设 | 结论 |
|---|---|
| URL 编码 / 二次编码 | ❌ 回显正确（RSS channel title = `必应：美团外卖 客服电话`），11-12 的二次编码 bug 未复发 |
| 首页参数（`form/pq/sc/cvid`） | ❌ 用户浏览器的完整 URL 与精简参数结果一致 |
| User-Agent | ❌ 一直是浏览器 UA |
| **Cookie** | ❌ 无 cookie ×3 与有 cookie（先 GET 首页拿 11 个）×3，size 与条目逐字节相同 |
| RSS vs HTML 通道 | ❌ 降级时两者条目完全一致（HTML 104KB / RSS 3.5KB，内容一样） |
| 查询词本身召回差 | ❌ 恢复后同一个词立刻正常 |
| 短时高频 | ⚠️ 连发 8 次、再灌 10 次 GBK 乱码请求，均未复现 |

**真实原因：被反爬系统判成 bot，返回降级结果（shadow SERP）**

- bing.com 由 **Akamai Bot Manager** 保护（`Passive edge · HTTP 200 · 需 TLS match`）：
  **不封禁、不报错，只返回过滤过的低质量结果**。
- 判定依据在 **TLS 握手层（JA3/JA4 指纹）**，不在 HTTP 头：
  > "TLS 指纹识别完全位于 HTTP 标头之下，这就是为什么完美的标头配上脚本化的 TLS 栈仍然会被标记。"
- 这解释了为什么换 UA、带 cookie 全部无效 —— **我们的 ClientHello 不像 Chrome**，
  而 C++ 走 OpenSSL 的握手指纹与浏览器差得更远。

**识别判据：只有"条数"可用，而且只能抓重度降级**

2026-11-13 又跑了一批正常查询统计分布，结论是 `size` 完全不能当判据：

| 查询（都是正常结果） | items | size |
|---|---|---|
| 肯德基 订餐电话 | 10 | **3294** |
| 深圳 天气 | 10 | **3724** |
| 嵊泗列岛 2019 台风 米娜 损失 | 10 | **3717** |
| JZShellPolicy::isWithinWorkspace | 10 | **3583** |
| 美团 客服电话 | 10 | 4161 |
| python asyncio | 10 | 4174 |

正常范围 **3.2–4.2KB**，与降级样本的 **3494** 完全重叠 —— 用 `size < 4KB` 会把上面一半正常查询误判。
（先前草稿写过"size < 4KB"，是拿单个样本推的，已废弃。）

`item` 数才是有区分度的：正常几乎恒定 **9–10 条**，重度降级 **2–3 条**。所以：

```
item 数 < 5   →   重度降级（可以重试）
item 数 ≥ 8   →   无法判断（见下）
```

**但降级是渐变的，中度降级躲得过这个判据。** 同一批 10 条结果的查询里：

| 查询 | items | size | 首条 |
|---|---|---|---|
| `如何用 find -delete 删除空目录` | 10 | 3812 | **如何（汉语词语）_百度百科** ← 按首字"如何" |
| `jzframework JZLocalSocketServer listen 实现` | 9 | 3367 | 搜狗输入法常见问题 |
| `Windows MSYS /proc/stat btime 含义` | 10 | 3808 | Windows - Microsoft 下载中心 |
| `JZShellPolicy::isWithinWorkspace` | 10 | 3583 | 企业微信有多个企业怎么删除 |

这四条都是**条数正常、体积正常、内容无关**。除"按首字命中"外没有机械特征，
只能靠模型看标题自己发现 —— 这与 §1.1「不做相关性启发式」的结论一致（用词面重合度判，误判率高）。

**处理**：

- 能做：`item < 5` 时**隔 1–2s 重试一次**（重度降级常是短时的，重试有效；正常查询不会掉到 5 条以下，误判代价仅一次请求）。
- 做不到：中度降级无法自动识别，只能把结果原样给模型，由它看标题判断可信度。
- 根本解：换搜索 API（§3），它们卖的就是"住宅 IP + 无头浏览器集群"这套反爬基础设施。
- **不要**为此引入无头浏览器（体积/维护不值），也**不要**试 cookie —— 已实测排除。

---

## 3. 可选方案与价格（每千次成功请求）

| 服务 | 价格 | 接口 | 中文 | 备注 |
|---|---|---|---|---|
| **DeepSeek Search** | **¥5**（≈$0.75） | POST `/v1/tools/search` | 好 | 2026-10-17 上线、11-01 起计费；失败/空结果/超时**不计费**；5 QPS；query ≤128 字符；返回 1–50 条；字段含标题/链接/摘要/长摘要/日期/重排序分数/来源权威度 |
| DeepSeek Fetch | ¥5 | POST `/v1/tools/fetch` | 好 | 单页 → Markdown |
| Kimi 联网 Basic | ¥10 | 私有 | 好 | Pro 版 ¥15 |
| 博查 Web Search | ¥36 | POST | 好 | 1,000 次免费试用 3 个月；大额包 ¥25–32/千次 |
| Brave Search | $5（≈¥36） | GET | 一般 | 每月含 $5 免费额度（≈1,000 次） |
| SerpAPI | $15（Developer $75/5,000） | GET | 好 | 包月限次：$25/1k、$75/5k、$150/15k、$275/30k |
| OpenAI / Anthropic Web Search | $10 | 私有 | 好 | 结果 token 另计 |
| 自建抓取（搜狗） | ≈¥0 | — | 好 | 秒级限流、验证码、维护成本、合规灰色 |

**为什么搜索 API 比"模型自带搜索"贵**：它卖的是实时抓取基础设施（住宅 IP 池、无头浏览器集群、
验证码破解、去重与索引），成本结构和推理 token 完全不是一个量级。DeepSeek 能压到 ¥5/千次，
是因为它用的是自家引擎/索引，且把失败请求排除了。

---

## 4. 接入方式

### 4.1 方式 A：纯配置（适用于返回 `results` / `RelatedTopics` / 顶层数组 / RSS 的服务）

```json
"network": {
    "enabled": true,
    "searchUrl": "https://your-search-api/search?q={q}&n=8"
}
```

模板里的 `{q}` 会被替换成 URL 编码后的查询词。**零代码改动**。

### 4.2 方式 B：接 DeepSeek（需少量代码）

DeepSeek 是 POST + JSON body + Bearer 鉴权，现有工具不支持，需要加：

1. `JZWebSearchTool` 增加 POST 能力（配置里给 `searchMethod` / `searchBody` / 鉴权头）；
2. `parseSearch()` 增加 DeepSeek 的返回结构（按其 Schema 取 `results` 数组）。

建议的配置形状（待实现）：

```json
"network": {
    "enabled": true,
    "searchUrl": "https://api.deepseek.com/v1/tools/search",
    "searchMethod": "POST",
    "searchToken": "sk-****",                                  // 复用 DeepSeek 的 key
    "searchBody": "{\"query\":\"{q}\",\"max_results\":8}"
}
```

请求体里的 `{q}` 同样按 URL 编码替换；成功判定：HTTP 200 且结果数组非空（空结果不计费）。

### 4.3 已删除：本地聚合服务（`web_search_server`）

曾有一版 Python 聚合服务（包搜狗 + 限流 + 缓存 + 跳转还原），用于在默认源不可用时兜底。
2026-11-12 确认**必应 RSS 直连可用**（已内置在 C++ 工具里）后删除：
它的核心价值是"绕开必应降级"，而那个前提本身是误判。

需要缓存/限流时再回来参考它的设计（同 query 缓存 300s、全局 1.2s 间隔、验证页识别）；
但当前场景下 C++ 侧的一次 GET 已经够用。

---

## 5. 推荐路线

1. **已落地（默认）**：必应 RSS 单源，零依赖、无需 Key。三个必须记住的点：
   - 带 `setlang=zh-CN&cc=CN`、请求头 `Accept-Language: zh-CN`；
   - 跟随 `www.bing.com → cn.bing.com` 的 302（`setRedirectPolicy(AllAllowed)`）；
   - **会偶发影子结果**（§2.1）：`item < 5` 时按重度降级处理，隔 1–2s 重试一次
     （正常查询恒为 9–10 条，这个判据误判率低；`size` 不能用，正常与降级完全重叠）。
     中度降级（10 条但按首字命中）无法自动识别。cookie / UA / 参数都救不了（已实测），根因是 TLS 指纹。
2. **需要更高频/更稳时**：接 DeepSeek Search API（¥5/千次，中文好、失败不计费、国内直连）；
3. **还可以补**（与源无关）：
   - **结果缓存**：同一 `query + num` 在 N 分钟内直接返回缓存，不消耗请求；
   - ~~相关性校验~~：**已否决**。曾按"查询词是否出现在结果里"判源降级，实测误判率高
     （「深圳 明天 天气 温度」命中"深圳"就放行、返回深圳市百科；「深圳天气 11月13日 预报 温度」
     因整串词不常见而被误报不可用）。结果好不好，交给模型看标题判断。
4. **不建议**：百度抓取（第 2 次就验证码）；
   高频抓搜狗（第 5 次 antispider）；装无头浏览器解决搜索（不需要，必应结果在初始 HTML 里）。

---

## 6. 附：排查手段

```bash
# 直接看某个源返回什么（RSS）
curl -s -G "https://www.bing.com/search" \
     --data-urlencode "q=最大的冬瓜" --data-urlencode "format=rss" --data-urlencode "count=15"

# 看是否被验证页拦截（搜狗/百度）
curl -s -D - -o /dev/null "https://www.sogou.com/web?query=test"
# 302 -> www.sogou.com/captcha/  = 已限流
```

> ⚠ **Windows（Git Bash）下上面的 curl 命令不可信**：`--data-urlencode` 会把中文按
> **GBK** 编码发出去（URL 里出现 `%c3%c0%cd%c5...`），必应收到乱码 → 返回完全无关的结果
> （实测拿到 GitHub c3c、比亚迪客车、Cloud9）。这是**排查工具的坑**，会让人误判成"源全坏了"。
> 用 python/requests，或先把 query 编码成 UTF-8 再发。

排查顺序：① 换不换源都是垃圾 → 源的问题；② 状态码 302/200 但只有几 KB → 验证页；
③ 结果标题与查询词零交集 → 源降级或查询被截断；④ 200 + 结构正常但条数只有 2–3 条、
体积 3.5KB 左右，且**过一小时自己就好了** → 影子 SERP（§2.1），重试而不是换源。

判"是谁的问题"的最快方法：看 RSS channel 的 `<title>`（必应会回显查询词）。
回显正确 = 编码没问题，问题在必应侧的判定；回显是 `%E6%B7%B1...` 这种字面量 = 我们自己的编码 bug。

---

## 7. 历史

- 2026-11-13（补）：**判据修正**。草稿里写的 `size < 4KB` 是拿 1 个降级样本推的，实测 10 个正常查询
  size 落在 3.2–4.2KB，与降级样本 3494 完全重叠，那个判据会误伤一半正常查询，已废弃。
  同时发现**降级是渐变的**：同批查询里有 4 条 `items=10, size≈3.7KB` 但内容无关
  （`如何用 find -delete 删除空目录` → 「如何（汉语词语）_百度百科」）。
  所以只有 `items < 5` 能判"重度降级"，中度只能靠模型看标题。
- 2026-11-13：**确认影子 SERP 真实存在**（§2.1）。起因是 cli-2 的任务「找一下深圳订餐电话」
  连搜 4 次全失败（返回「美」「饿」这类词典条目），模型只好凭记忆报号码。
  排查发现同一串参数 20 分钟内结果不同（3494/「美」↔ 4367/「服务热线-美团」），
  并逐条排除了编码、参数、UA、cookie、RSS-vs-HTML、查询词、短时高频。
  读到反爬资料后定性：bing.com 由 Akamai Bot Manager 保护，按 **TLS 握手指纹（JA3/JA4）**
  判 bot，命中后**返回 200 + 过滤过的低质量结果**（行业内叫 shadow SERP），不封禁。
  教训与前两次同类：判"源坏了"之前，先确认自己发出去的到底是什么（这次是 TLS 指纹，
  前两次也各有根因）。文档补了 Windows curl 的 GBK 坑。
- 2026-11-12（晚·四）：**去掉 HTML 兜底，只保留必应 RSS**。RSS 一直可用，HTML 是同结果的 18 倍体积；
  多一条解析路径就多一处会坏的地方（这次二次编码 bug 排查时，两条路径还互相干扰了判断）。
  删除 `jzParseBingHtmlResults` 及 185 行辅助函数、`test/net/test_search_parse.cpp` 与其 ctest。
- 2026-11-12（晚·三）：**根因其实是自己的 URL 编码 bug**（`JZUrlQuery::fromString` 不解码而
  `toString` 编码 → 任何 URL 重建一次就二次编码）。必应 302 到 `cn.bing.com` 时重建 URL，查询词
  变成字面 `%E6%B7%B1...`，必应读不懂 → 返回一批随机热点页。铁证：必应 RSS 回显
  「必应：%E6%B7%B1%E5%9C%B3%20%E5%A4%A9%E6%B0%94」。此前所有"源降级"结论都是这个 bug 的假象。
  修复在 `JZUrl.cpp`（`fromString` 加 `urlDecode`）。同时**移除相关性启发式判定**：
  实测「深圳 明天 天气 温度」命中"深圳"就放行、返回深圳市百科；「深圳天气 11月13日 预报 温度」
  因整串词不常见被误报不可用 —— 两头都不可靠，只保留硬错误（非 2xx / 验证页 / 解析为空）判定。
- 2026-11-12（晚）：修正"HTML 不可用"的结论，新增 `jzParseBingHtmlResults()` +
  离线单测 `test/net/test_search_parse.cpp`（真实页面 105 KB → 10 条全对）。
- 2026-11-12：确认 Bing RSS 降级为词典/无关结果；实测百度第 2 次即验证码、
  搜狗第 5 次触发 antispider；确认 DeepSeek 开放独立 Search/Fetch API（¥5/千次）。
