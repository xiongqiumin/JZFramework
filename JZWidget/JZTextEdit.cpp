#include "JZTextEdit.h"
#include "JZTextImageMap.h"

JZ_OBJECT_IMPL(JZTextEdit, JZAbstractTextEdit)

JZTextEdit::JZTextEdit(JZWidget* parent)
    : JZAbstractTextEdit(parent, false)
{
}

JZSize JZTextEdit::sizeHint() const
{
    return JZSize(200, 70);
}

void JZTextEdit::setLineWrap(bool wrap)
{
    setLineWrapMode(wrap);
}

bool JZTextEdit::lineWrap() const
{
    return isLineWrapMode();
}

// ---- 图文混排: 全部转发基类实现 ----

void JZTextEdit::appendImage(const JZImage& image, const std::string& key,
                             int width, int height)
{
    JZAbstractTextEdit::appendImage(image, key, width, height);
}

void JZTextEdit::insertImage(const JZImage& image, const std::string& key,
                             int width, int height)
{
    JZAbstractTextEdit::insertImage(image, key, width, height);
}

// 纯文本: 图片码点 -> "[图片]" (供下发 AI / 写日志)。
// 逐码点遍历: 私有域码点直接替换, 其余原样保留。
std::string JZTextEdit::toPlainText() const
{
    const std::string src = text();
    const JZString s = JZString::fromStdString(src);

    std::string out;
    JZString run;                       // 攒连续的非图片码点, 成批转 UTF-8
    for (int i = 0; i < s.size(); ++i) {
        if (JZTextImageMap::isImageCode(s[i].toUnicode())) {
            if (!run.isEmpty()) {
                out += run.toStdString();
                run.clear();
            }
            out += "[图片]";
        } else {
            run.push_back(s[i]);
        }
    }
    if (!run.isEmpty())
        out += run.toStdString();
    return out;
}

std::string JZTextEdit::toMarkedText() const
{
    return text();
}
