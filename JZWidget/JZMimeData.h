#ifndef JZ_MIME_DATA_H_
#define JZ_MIME_DATA_H_
#include <map>
#include <string>
#include <vector>

#include "JZByteArray.h"
#include "JZImage.h"

// 拖放携带的数据容器 (等价 Qt QMimeData, 这里做成轻量值对象: 可拷贝, 不带对象树语义)。
//
// 两层 API:
//   * 便捷项 —— text / urls / image: 覆盖拖放的绝大部分场景 (拖文本、拖文件、拖图片);
//   * 通用项 —— setData / data / hasFormat / formats: 按 MIME 名自由存取任意格式
//     (STGMEDIUM 里的原始数据按字节原样保存), 供自定义格式与"先看看有什么"用。
//
// 拖进来的数据由平台层 (Windows 下是 OLE 的 IDataObject) 一次性解析填充,
// 整个拖放过程发给控件的 dragEnter/dragMove/drop 事件共享同一份 JZMimeData。
//
// 内部存储就是"通用项"那张表; 三个便捷项是它之上的视图与约定 —— text/urls 落在表里,
// image 以 JZImage 对象单独持有, 但同样以自己的标准名出现在 formats() 里:
//   text  -> "text/plain"        (UTF-8 字节)
//   urls  -> "text/uri-list"     (每行一条本地路径)
//   image -> "application/x-jz-image"
// 之所以沿用这套标准名, 是为了让 formats()/data() 的自查结果与拖放惯用约定对得上。
class JZFRAMEWORK_API JZMimeData
{
public:
    JZMimeData();

    // ---- 通用格式 ----

    // 按 MIME 名存一份原始数据 (同名覆盖)。format 为空串则忽略。
    void setData(const std::string& format, const JZByteArray& data);
    // 取指定格式的原始数据; 没有则返回空 JZByteArray。
    JZByteArray data(const std::string& format) const;
    bool hasFormat(const std::string& format) const;
    // 当前持有的全部格式名 (含下面三个便捷项的标准名; 顺序按名字典序)。
    std::vector<std::string> formats() const;
    void removeFormat(const std::string& format);
    void clear();
    bool isEmpty() const;

    // ---- 文本 ----
    bool hasText() const;
    void setText(const std::string& text);
    // 无 "text/plain" 时回落到 "text/plain;charset=utf-8" (拖/拷遇到过这种写法)。
    std::string text() const;

    // ---- 文件路径 ----
    // 存的是**本地路径** (UTF-8), 不是 RFC 的 file:// URL —— 资源管理器/浏览器拖进来的
    // 文件路径已经由平台层解出, 业务直接拿去 fopen / JZImage(path) 即可。
    bool hasUrls() const;
    void setUrls(const std::vector<std::string>& paths);
    std::vector<std::string> urls() const;

    // ---- 图像 ----
    // hasImage: 拖的是位图数据 (CF_DIB/CF_HBITMAP 之类), 或者 urls 里第一条是可识别的
    //   图片文件 (.png/.jpg/.jpeg/.bmp/.gif/.webp/.tga) —— 后者是 Qt 没有的便利扩展:
    //   资源管理器拖图片进来时通常只有文件路径, 业务最想要的其实是那张图。
    // image: 优先返回内联位图; 没有则从 urls 的第一条解码 (失败得到 isNull()==true 的空图,
    //   结果会被缓存, 重复调用不重复读盘)。
    bool hasImage() const;
    void setImage(const JZImage& image);
    JZImage image() const;

    // 三个便捷项的标准 format 名 (与上面的约定一致)
    static const std::string& textFormat();
    static const std::string& urlsFormat();
    static const std::string& imageFormat();

private:
    void cacheImage() const;   // image() 的懒解码: 结果写进下面两个 mutable 缓存

    std::map<std::string, JZByteArray> m_data;

    // 图像以对象形式持有 (不进 m_data —— 那是原始字节的表): setImage 直接填上,
    // 文件系统拖进来的图片则由首次 image() 解码得到。hasImage() 据此判定。
    mutable JZImage m_imageCache;
    mutable bool    m_imageCached = false;
};

#endif // !JZ_MIME_DATA_H_
