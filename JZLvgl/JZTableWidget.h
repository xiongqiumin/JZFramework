#ifndef JZ_LVGL_TABLE_H_
#define JZ_LVGL_TABLE_H_

#include "JZWidget.h"

class JZTable : public JZWidget
{
    JZ_OBJECT_DECLARE(JZTable)

public:
    explicit JZTable(JZWidget* parent = nullptr);

    void setSize(uint16_t cols, uint16_t rows);
    void setColWidth(uint16_t col, lv_coord_t w);
    void setCellValue(uint16_t row, uint16_t col, const char* txt);
};

#endif
