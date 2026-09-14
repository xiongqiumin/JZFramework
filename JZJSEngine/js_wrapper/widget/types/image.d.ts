// JZImage —— RGBA8888 图像(全局类; registImage + js_JZImage_ctor)。
//
// 三个入口:
//   1) new JZImage()            空图(isNull() === true, 不能设像素/不能当离屏画笔目标)
//   2) new JZImage(file)        从文件解码(按扩展名: .png/.bmp/.tga/.jpg/.jpeg)
//   3) new JZImage(bytes)       从内存解码(Uint8Array)
//   4) JZImage.create(w, h)     **造图**入口(挂在构造器上的静态函数): 新建纯透明(alpha=0)画布
//
// 离屏绘制: const img = JZImage.create(200, 100);
//           const p = new JZPainter(img); ...; p.end();     // 见 painter.d.ts
// 显示:     new JZImageLabel().setImage(img)

/** @final */
declare class JZImage {
  /** 空图(isNull() === true); 不能设尺寸/像素 */
  constructor();
  /** 从文件解码; 失败得到空图 */
  constructor(file: string);
  /** 从内存字节解码; 失败得到空图 */
  constructor(bytes: Uint8Array);

  /** 新建纯透明(RGBA 全 0)画布 —— 离屏画笔的目标 */
  static create(w: number, h: number): JZImage;

  width(): number;
  height(): number;
  /** 无像素数据 */
  isNull(): boolean;

  /** 按扩展名推断编码格式(.png/.bmp/.tga/.jpg/.jpeg); 无像素或写盘失败返回 false */
  save(file: string): boolean;
  /** 缩放为新图(拉伸), 不改本对象; 失败返回空图 */
  scaled(w: number, h: number): JZImage;
}

// 未暴露(C++ 有): decode / fromRGBA / fromData / encode / data。
