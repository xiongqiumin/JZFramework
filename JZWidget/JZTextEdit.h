#ifndef JZ_WIDGET_TEXTAREA_H_
#define JZ_WIDGET_TEXTAREA_H_
#include "JZAbstractTextEdit.h"

// ==================== 多行文本编辑框 ====================
// 全部编辑/绘制/输入法逻辑在 JZAbstractTextEdit (多行模式), 本类只提供多行语义:
// 自动换行开关与默认尺寸。
class JZFRAMEWORK_API JZTextEdit : public JZAbstractTextEdit
{
    JZ_OBJECT_DECLARE(JZTextEdit)

public:
    explicit JZTextEdit(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;

    // 自动换行; 关闭时超出宽度的内容靠水平滚动查看
    void setLineWrap(bool wrap);
    bool lineWrap() const;

    // ---- 图文混排 ----
    // 图片在文本流里占**一个私有域码点**, 因此就是一个普通字符: 光标/退格/
    // 选区/测宽全部天然按 1 个单元处理。像素与尺寸登记在 JZTextImageMap 单例。
    // key 用于标识图片 (如文件路径), 为空时自动生成唯一 key。
    void appendImage(const JZImage& image, const std::string& key = std::string(),
                     int width = 0, int height = 0);
    void insertImage(const JZImage& image, const std::string& key = std::string(),
                     int width = 0, int height = 0);

    // 纯文本 (图片码点替换成 "[图片]"), 适合写盘 / 发给 AI
    std::string toPlainText() const;
    // 含图片码点的原始文本 (用于持久化后原样恢复)
    std::string toMarkedText() const;
};

#endif
