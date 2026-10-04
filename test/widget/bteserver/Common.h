#ifndef BTE_COMMON_H_
#define BTE_COMMON_H_

// ---------------------------------------------------------------------------
// Common.h —— 对应原版 bteserver/common/Common.h(全局常量 + UiHelper)。
//
// 原版是 `const int MAX_SUB_DEVICE = 4;` 这种文件作用域常量(头里定义, 各 TU 各一份);
// 这里用 enum 是为了让它们能做数组/模板实参, 语义一样。
// 多出来的一个: HEART_TIMEOUT_SEC —— 原版这个值硬编码在 onHeartTime() 里(60 秒)。
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

class JZTableWidget;

const int MAX_SUB_DEVICE  = 4;
const int MAX_CHANNEL_NUM = 8;
const int MAX_DEVICE_ITEM = (MAX_SUB_DEVICE * MAX_CHANNEL_NUM);   // 32
const int MAX_SPLIT_LEVEL = 15;                                   // 分选级别数上限(分选还没搬完)

// 心跳超时(原版 onHeartTime: heartTime.secsTo(cur) >= 60 就当掉线)
const int HEART_TIMEOUT_SEC = 60;

// ---------------------------------------------------------------------------
// UiHelper —— 对应原版 common/Common.h 里的同名类(目前只有 SetTable)。
// 原版 SetTable 一口气设了 6 项(列数/表头/单选/整行选中/禁止编辑/末列拉伸/隐藏行号),
// 引擎的 JZTableWidget 目前只有"整行选中(setSelectionBehavior)"这一项对得上,
// 其余(编辑触发/末列拉伸/行号列)引擎还没有 —— 不造假接口, 缺的就在注释里记着。
// ---------------------------------------------------------------------------
class UiHelper
{
public:
    static void SetTable(JZTableWidget* table, const std::vector<std::string>& list);
};

#endif // BTE_COMMON_H_
