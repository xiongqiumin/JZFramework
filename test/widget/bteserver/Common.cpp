#include "Common.h"
#include "JZTableWidget.h"

void UiHelper::SetTable(JZTableWidget* table, const std::vector<std::string>& list)
{
    if (!table)
        return;

    table->setColumnCount((int)list.size());
    table->setHorizontalHeaderLabels(list);
    table->setSelectionBehavior(JZTableWidget::SelectRows);   // 原版: setSelectionBehavior(SelectRows)

    // 原版还有这 4 项, 引擎的 JZTableWidget 目前没有对应接口:
    //   setSelectionMode(SingleSelection) / setEditTriggers(NoEditTriggers)
    //   horizontalHeader()->setStretchLastSection(true) / verticalHeader()->hide()
}
