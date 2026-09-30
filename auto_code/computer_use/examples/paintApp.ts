// paintApp.ts
//
// 微软画图（MS Paint）的语义树模型，表示为一个 TS 类。
//
// 约定（与 computeruse 包设计一致）：
//   - 本文件只"描述"界面结构（ui 树），不关心它是怎么被探索/生成的。
//   - 控件以具名字段暴露（paint.btnEllipse），存在性在建模期确定，执行期不再做标签查找。
//   - Button.click() 只做两件事：判按钮是否存在(模板匹配)、判点击是否失败；失败抛异常。
//     父链（如"主页"选项卡）的激活由【脚本主动】调用 ensureActivated()，click 内部不激活。
//
// 依赖 JZJSEngine/js_wrapper/computeruse 包（已实现：图片锚点式 Button）。

import {
  App, Tab, Button, Canvas,
  key, type as typeText, wait,
} from 'computeruse';

// 注：下面的定位策略与 region 小区域都来自"录制/探索期"的产物
// （截图 → VLM 标出锚点 → 决定每个控件用什么策略找：文字按钮用 ocr、图标用 image、
//  颜色块用 color、复杂图标用 yolo）。这里是示意值，真实值由录制阶段生成。

export class PaintApp extends App {
  // —— 选项卡（文字标签，OCR 找）——
  tabHome: Tab = this.addTab('主页');
  tabView: Tab = this.addTab('查看');

  // —— 工具 / 形状（主页选项卡下）——
  // 铅笔/画笔是图标 → image 模板；形状是文字按钮 → ocr；椭圆是画廊图标 → image。
  btnPencil:  Button = this.tabHome.addButton('铅笔', { kind: 'image', image: 'anchors/pencil.png', region: { x: 0.02, y: 0.12, w: 0.08, h: 0.06 } });
  btnBrush:   Button = this.tabHome.addButton('画笔', { kind: 'image', image: 'anchors/brush.png',  region: { x: 0.10, y: 0.12, w: 0.08, h: 0.06 } });
  btnShapes:  Button = this.tabHome.addButton('形状', { kind: 'ocr',   text: '形状',                region: { x: 0.30, y: 0.12, w: 0.10, h: 0.06 } });
  btnEllipse: Button = this.tabHome.addButton('椭圆', { kind: 'image', image: 'anchors/ellipse.png', region: { x: 0.30, y: 0.19, w: 0.10, h: 0.06 } });

  // —— 颜色（colorFind 按颜色）——
  btnColor1: Button = this.addButton('颜色1', { kind: 'color', color: '#000000', region: { x: 0.78, y: 0.03, w: 0.04, h: 0.04 } });  // 前景色（轮廓）
  btnColor2: Button = this.addButton('颜色2', { kind: 'color', color: '#FFFFFF', region: { x: 0.82, y: 0.03, w: 0.04, h: 0.04 } });  // 背景色（填充）
  btnRed:    Button = this.addButton('红色',   { kind: 'color', color: '#FF0000', region: { x: 0.78, y: 0.09, w: 0.04, h: 0.04 } });  // 调色板里的红色色板
  // 复杂图标可改用 yolo：{ kind:'yolo', model:'anchors/icons.pt', label:'red', region:{...} }

  // —— 画布（区域节点，按坐标拖拽）——
  canvasMain: Canvas = this.addCanvas('画布');

  // 打开画图：通用 OS 启动动作（非树内控件，用底层 primitive）。
  // 新启动的 Paint 默认在前台；若需可加 focusWindow('画图')。
  static async launch(): Promise<PaintApp> {
    const app = new PaintApp('Paint');
    await key('LWin');           await wait(300);
    await typeText('mspaint');   await wait(300);
    await key('Return');         await wait(2000);   // 等 Paint 起来
    return app;
  }
}
