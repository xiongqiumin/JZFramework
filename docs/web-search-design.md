# 网络搜索（web_search）现状与选型

> 2026-11-12 调研。结论先行：**必应 RSS 国内直连可用**（~6KB、10 条、链接直链），零依赖、无需
> API Key，已在 `web_search` 里作为唯一默认源。关键前提：带 `setlang=zh-CN&cc=CN`、跟随 302、
> **不要走代理**（出口 IP 会被判滥用，返回垃圾结果）。本文记录现状、实测数据、可选方案与接入方式。

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
| **走代理** | ❌ 不可用 | 经 `101.32.19.163:7001`（香港出口）：RSS 返回一堆 Telegram、HTML 搜 "python" 返回 Lidl 超市 → 出口 IP 被必应判滥用。**上一版把"代理下的垃圾"误判成 RSS 通道降级**，实际国内直连正常 |
| **百度** | ❌ 不适合自动化 | 无 cookie 直接 302；先访问首页拿 `BAIDUID` + Referer 后第一次 200，**第二次就 302 到 `wappass.baidu.com/...tuxing_v2`（图形验证码）**，且粘性 |
| **搜狗** | ⚠️ 单次可用，限流严 | 浏览器 UA 下第 1–4 次（间隔 300ms）都是 200 / ~420KB / 1.3–1.5s；**第 5 次起**返回 200 + 3.5KB `antispider` 页，随后 302 到 `www.sogou.com/captcha/`；无 UA 时 200 但只有 5.4KB 提示页 |
| **DuckDuckGo API** | ❌ 不可达 | `code=000`（需代理，且 `/v1` 免费接口质量一般） |
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
它的核心价值是"绕开必应降级"，而那个前提本身是误判（降级其实是代理出口造成的）。

需要缓存/限流时再回来参考它的设计（同 query 缓存 300s、全局 1.2s 间隔、验证页识别）；
但当前场景下 C++ 侧的一次 GET 已经够用。

---

## 5. 推荐路线

1. **已落地（默认）**：必应 RSS 单源，零依赖、无需 Key。三个必须记住的点：
   - 带 `setlang=zh-CN&cc=CN`、请求头 `Accept-Language: zh-CN`；
   - 跟随 `www.bing.com → cn.bing.com` 的 302（`setRedirectPolicy(AllAllowed)`）；
   - **不要走代理**（出口 IP 会被判滥用，返回无关结果）。
2. **需要更高频/更稳时**：接 DeepSeek Search API（¥5/千次，中文好、失败不计费、国内直连）；
3. **还可以补**（与源无关）：
   - **结果缓存**：同一 `query + num` 在 N 分钟内直接返回缓存，不消耗请求；
   - ~~相关性校验~~：**已否决**。曾按"查询词是否出现在结果里"判源降级，实测误判率高
     （「深圳 明天 天气 温度」命中"深圳"就放行、返回深圳市百科；「深圳天气 11月13日 预报 温度」
     因整串词不常见而被误报不可用）。结果好不好，交给模型看标题判断。
4. **不建议**：用代理抓必应（出口 IP 会被判滥用，返回无关结果）；百度抓取（第 2 次就验证码）；
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

排查顺序：① 换不换源都是垃圾 → 源的问题；② 状态码 302/200 但只有几 KB → 验证页；
③ 结果标题与查询词零交集 → 源降级或查询被截断。

---

## 7. 历史

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
- 2026-11-12（晚·二）：**再修正**。RSS 通道其实没降级——上一版"RSS 返回 Telegram"的样本是**走代理**
  抓的。直连复测 `冬瓜` / `python asyncio` / `上海天气` 三次，RSS 均 10 条全相关、仅 ~6KB。
  默认源定为「RSS 优先 → HTML 兜底」。
  **教训**：判"源坏了"之前必须先排除代理出口污染——共享代理 IP 被搜索引擎拉黑是常态。
- 2026-11-12（晚）：修正"HTML 不可用"的结论，新增 `jzParseBingHtmlResults()` +
  离线单测 `test/net/test_search_parse.cpp`（真实页面 105 KB → 10 条全对）。
- 2026-11-12：确认 Bing RSS 降级为词典/无关结果（含代理复测）；实测百度第 2 次即验证码、
  搜狗第 5 次触发 antispider；确认 DeepSeek 开放独立 Search/Fetch API（¥5/千次）。
