// draw_red_circle.ts
//
// 打开画图并画一个红色圆：完全通过 paintApp 的 ui 树控制，无 OCR 查找。
// 运行方式：由 JZJSEngine 加载执行（依赖 computeruse 包落地后）。

import { PaintApp } from './paintApp';

async function main(): Promise<void> {
  const paint = await PaintApp.launch();          // 打开画图

  // 任务逻辑放在脚本里（不在 app 模型内）：通过 ui 树控件画一个红色圆
  // 1) 脚本主动激活父链：先切到"主页"选项卡，Ribbon 才显示其按钮
  await paint.tabHome.ensureActivated();

  // 2) 选椭圆形状（若实际藏在"形状"画廊下，则先 paint.btnShapes.ensureActivated()）
  await paint.btnEllipse.click();

  // 3) 设红色：打开前景色板 → 点红色色板
  await paint.btnColor1.click();
  await paint.btnRed.click();
  //    想要实心红圆可再设填充色：paint.btnColor2.click(); paint.btnRed.click();

  // 4) 在画布上拖出一个圆（坐标是**物理像素** ⇒ 按画布控件当前几何取中心，半径 = 短边 15%）
  const c = await paint.canvasMain.locate();
  if (!c) throw new Error('[CU] 找不到画布控件');
  const r = Math.min(c.w, c.h) * 0.15;
  await paint.canvasMain.drag({ x: c.x + c.w / 2 - r, y: c.y + c.h / 2 - r },
                              { x: c.x + c.w / 2 + r, y: c.y + c.h / 2 + r });

  console.log('[draw_red_circle] done');
}

main().catch((e) => {
  console.error('[draw_red_circle] failed:', e);
  throw e;
});
