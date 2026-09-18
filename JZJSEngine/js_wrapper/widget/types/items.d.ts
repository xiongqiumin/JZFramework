// (已废弃) 原 JZTableWidgetItem / JZListWidgetItem 的 JS 声明 —— 那两个 item 类**不暴露给 JS**,
// 且从未接线(属死代码)。列表/表格的 JS 接口只有"值级"接口 + **视图镜像**:
//   * 值级: JZTableWidget 的 setRows/rows/appendRow/… , JZListWidget 的 setItems/items/appendItem/…
//   * 视图镜像: `const dv = table.dataView()` —— 见 views.d.ts 与 dataview.d.ts。
// 本文件保留为空壳, 只为兼容老工程里可能存在的 `/// <reference ... items.d.ts>`。
export {};
