#ifndef JZ_LVGL_LAYOUT_H_
#define JZ_LVGL_LAYOUT_H_

#include "lvgl.h"
#include <vector>

struct JZLayout
{
    // 快速构造
    static JZLayout vbox(int pad = 0, int gap = 0);
    static JZLayout hbox(int pad = 0, int gap = 0);
    static JZLayout grid(const std::vector<lv_coord_t>& cols,
                             const std::vector<lv_coord_t>& rows = {},
                             int pad = 0, int gap = 0);

    void apply(lv_obj_t* obj) const;
    
    uint16_t type = LV_LAYOUT_NONE;

    lv_flex_flow_t flexFlow = LV_FLEX_FLOW_ROW;
    lv_flex_align_t flexMain = LV_FLEX_ALIGN_START;
    lv_flex_align_t flexCross = LV_FLEX_ALIGN_START;
    lv_flex_align_t flexTrack = LV_FLEX_ALIGN_START;

    std::vector<lv_coord_t> colDesc;
    std::vector<lv_coord_t> rowDesc;

    int pad = 0;
    int gap = 0;
};

#endif // JZ_LVGL_LAYOUT_H_
