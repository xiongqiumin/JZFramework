#include "JZLvglLayout.h"

JZLvglLayout JZLvglLayout::vbox(int pad, int gap)
{
    JZLvglLayout l;
    l.type = LV_LAYOUT_FLEX;
    l.flexFlow = LV_FLEX_FLOW_COLUMN;
    l.pad = pad;
    l.gap = gap;
    return l;
}

JZLvglLayout JZLvglLayout::hbox(int pad, int gap)
{
    JZLvglLayout l;
    l.type = LV_LAYOUT_FLEX;
    l.flexFlow = LV_FLEX_FLOW_ROW;
    l.pad = pad;
    l.gap = gap;
    return l;
}

JZLvglLayout JZLvglLayout::grid(const std::vector<lv_coord_t>& cols,
                                 const std::vector<lv_coord_t>& rows,
                                 int pad, int gap)
{
    JZLvglLayout l;
    l.type = LV_LAYOUT_GRID;
    l.colDesc = cols;
    l.rowDesc = rows;
    l.pad = pad;
    l.gap = gap;
    return l;
}

void JZLvglLayout::apply(lv_obj_t* obj) const
{
    lv_obj_set_layout(obj, type);

    if (type == LV_LAYOUT_FLEX) {
        lv_obj_set_flex_flow(obj, flexFlow);
        lv_obj_set_flex_align(obj, flexMain, flexCross, flexTrack);
    }

    if (type == LV_LAYOUT_GRID) {
        if (!colDesc.empty()) {
            auto cols = colDesc; cols.push_back(LV_GRID_TEMPLATE_LAST);
            lv_obj_set_grid_dsc_array(obj, cols.data(), nullptr);
        }
        if (!rowDesc.empty()) {
            auto rows = rowDesc; rows.push_back(LV_GRID_TEMPLATE_LAST);
            lv_obj_set_grid_dsc_array(obj, nullptr, rows.data());
        }
    }

    if (pad > 0) lv_obj_set_style_pad_all(obj, pad, 0);
    if (gap > 0) lv_obj_set_style_pad_gap(obj, gap, 0);
}
