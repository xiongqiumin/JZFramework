#ifndef JZ_LVGL_TABLE_H_
#define JZ_LVGL_TABLE_H_

#include "JZLvglWidget.h"

class JZLvglTable : public JZLvglWidget
{
public:
    explicit JZLvglTable(JZLvglWidget* parent = nullptr);

    void setSize(uint16_t cols, uint16_t rows) { lv_table_set_row_cnt(m_lv, rows); lv_table_set_col_cnt(m_lv, cols); }
    void setColWidth(uint16_t col, lv_coord_t w) { lv_table_set_col_width(m_lv, col, w); }
    void setCellValue(uint16_t row, uint16_t col, const char* txt) { lv_table_set_cell_value(m_lv, row, col, txt); }
};

#endif
