// ---------------------------------------------------------------------------
// Common.js —— 对应 C++ common/Common.h + Common.cpp
//
// 全局常量与表格辅助。C++ 里这些常量是 include 即得的编译期常量, JS 侧改成
// 具名导出, 用法: import { MAX_DEVICE_ITEM } from './Common.js';
//
// Qt -> JS: QTableWidget/QStringList 未在 JS 侧存在, UiHelper::SetTable 只保留
// 结构与注释, 内部按 Qt 语义直译(QTableWidget -> 传入的表格对象)。
//
// 2026-09-14 去 Qt 兼容层: 传进来的 table 现在一定是真 JZTableWidget, 所以只调
// 引擎真有的接口(列数 / 表头文本); 下面这几项 Qt 特有的"显示开关"引擎没有对应接口,
// 而且都是非必须的显示行为 —— 保留调用点说明, 不再调不存在的接口:
//   setSelectionMode(SingleSelection) / setSelectionBehavior(SelectRows) / setEditTriggers(NoEditTriggers)
//   horizontalHeader().setStretchLastSection(true) / verticalHeader().hide()
// (引擎的表格默认就是"单行选中、不可编辑、没有行号表头"。)
// ---------------------------------------------------------------------------

export const MAX_SUB_DEVICE = 4;
export const MAX_CHANNEL_NUM = 8;
export const MAX_DEVICE_ITEM = (MAX_SUB_DEVICE * MAX_CHANNEL_NUM);

export const MAX_SPLIT_LEVEL = 15;

// UiHelper (Common.h)
export class UiHelper
{
    // static void SetTable(QTableWidget *table, QStringList list);
    static SetTable(table, list)
    {
        table.setColumnCount(list.length);
        table.setHorizontalHeaderLabels(list);
    }
}
