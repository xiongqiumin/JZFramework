// JZImage —— RGBA8888 图像(全局类; JZJSWidgetModule::registImage)。
//
// 现状: JS 侧只拿得到**已有的图**(JZPushButton.icon() 之类返回值), 没有可用的"造图"入口 ——
//   * `def("create", &JZImage::create)`: 把一个**静态**工厂当实例方法注册, 实参位整体错开
//     (param0 期望 int 却收到对象), 无论怎么写都抛类型错误;
//   * decode / fromRGBA / fromData / encode / data 都没有暴露。
// 所以本文件只声明真正可用的成员; `new JZImage()` 只会得到一张空图(isNull() === true,
// 无像素) —— 它**不能**用于离屏画笔(new JZPainter(image) 要求尺寸有效)。
//
// (静态工厂是 C++ 侧的本意: JZImage.create(w, h, color) —— 待 C++ 绑定修正后再补进本文件。)

declare class JZImage {
  /** 空图(isNull() === true); 不能设尺寸/像素 */
  constructor();

  width(): number;
  height(): number;
  /** 无像素数据 */
  isNull(): boolean;

  /** 按扩展名推断编码格式(.png/.bmp/.tga/.jpg/.jpeg); 无像素或写盘失败返回 false */
  save(file: string): boolean;
  /** 缩放为新图(拉伸), 不改本对象; 失败返回空图 */
  scaled(w: number, h: number): JZImage;
}
