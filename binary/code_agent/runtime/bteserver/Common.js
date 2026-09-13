// ---------------------------------------------------------------------------
// Common.js —— 对应 C++ common/Common.h + Common.cpp
//
// 全局常量与表格辅助。C++ 里这些常量是 include 即得的编译期常量, JS 侧改成
// 具名导出, 用法: import { MAX_DEVICE_ITEM } from './Common.js';
//
// Qt -> JS: QTableWidget/QStringList 未在 JS 侧存在, UiHelper::SetTable 只保留
// 结构与注释, 内部按 Qt 语义直译(QTableWidget -> 传入的表格对象)。
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

        table.setSelectionMode(QAbstractItemView.SingleSelection); // 单击选择一行
        table.setSelectionBehavior(QAbstractItemView.SelectRows);  // 设置只能选择一行，不能多行选
        table.setEditTriggers(QTableWidget.NoEditTriggers);        // 禁止编辑
        table.horizontalHeader().setStretchLastSection(true);      // 末尾拉伸
        // table.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch); // 自适应

        table.verticalHeader().hide();     // 列标题隐藏
    }
}
