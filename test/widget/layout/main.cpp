// 布局 / sizeHint 回归测试
//
// 背景 (2026-09-09 修复): JZListWidget 未重写 sizeHint → 落到 JZWidget::sizeHint() 的 0×0。
// 它作为固定项 (stretch=0 + 默认 SizePolicy::Preferred ⇒ weight=0) 时,
// JZBoxLayout 的 "weight==0 严格保持 sizeHint" 会把宽度压成 0 —— 控件直接消失。
// 修法: 由 JZListWidget 自己提供 sizeHint (只有它知道自己该多大), 不动布局层。
#include "JZTest.h"
#include "JZApplication.h"
#include "JZWindow.h"
#include "JZWidget.h"
#include "JZPushButton.h"
#include "JZListWidget.h"
#include "JZLayout.h"

// 列表自己给出合理的期望尺寸
static bool list_sizehint_sane()
{
    JZListWidget list;
    list.appendItems({ "Label", "PushButton", "Slider" });

    JZSize sh = list.sizeHint();
    JZ_TEST_GT(sh.width(), 0);
    JZ_TEST_GT(sh.height(), 0);
    return true;
}

// 期望尺寸随内容变化; 行数封顶 (再多交给滚动)
static bool list_sizehint_follows_content()
{
    JZListWidget a;
    a.appendItem("x");
    JZListWidget b;
    b.appendItems({ "x", "y", "z" });
    JZ_TEST_GT(b.sizeHint().height(), a.sizeHint().height());

    JZListWidget wide;
    wide.appendItem("a-short-one");
    JZListWidget wider;
    wider.appendItem("a-much-much-longer-item-text");
    JZ_TEST_GT(wider.sizeHint().width(), wide.sizeHint().width());

    // 8 行封顶: 50 项不应比 8 项更高
    JZListWidget c8;
    for (int i = 0; i < 8; i++)
        c8.appendItem("item");
    JZListWidget c50;
    for (int i = 0; i < 50; i++)
        c50.appendItem("item");
    JZ_TEST_LE(c50.sizeHint().height(), c8.sizeHint().height());
    return true;
}

// HBox 固定项: 宽度 = sizeHint (不再被压成 0)
static bool hbox_fixed_uses_sizehint()
{
    JZWindow win;
    win.resize(800, 600);

    JZWidget* box = new JZWidget(&win);
    JZListWidget* list = new JZListWidget(box);
    list->appendItems({ "Label", "PushButton", "Slider" });
    JZWidget* right = new JZWidget(box);

    JZHBoxLayout* lay = new JZHBoxLayout(4, 4);
    lay->addWidget(list, 0);    // 固定宽
    lay->addWidget(right, 1);   // 占剩余
    box->setLayout(lay);
    box->resize(800, 400);
    box->updateLayout();

    JZ_TEST_GT(list->width(), 0);
    JZ_TEST_EQ(list->sizeHint().width(), list->width());   // weight==0: 严格取 hint
    JZ_TEST_GT(right->width(), 0);
    return true;
}

// VBox 固定项同理
static bool vbox_fixed_uses_sizehint()
{
    JZWindow win;
    win.resize(800, 600);

    JZWidget* box = new JZWidget(&win);
    JZListWidget* list = new JZListWidget(box);
    list->appendItems({ "a", "b", "c" });
    JZWidget* body = new JZWidget(box);

    JZVBoxLayout* lay = new JZVBoxLayout(4, 4);
    lay->addWidget(list, 0);    // 固定高
    lay->addWidget(body, 1);
    box->setLayout(lay);
    box->resize(800, 500);
    box->updateLayout();

    JZ_TEST_GT(list->height(), 0);
    JZ_TEST_EQ(list->sizeHint().height(), list->height());
    JZ_TEST_GT(body->height(), 0);
    return true;
}

// 回归守卫: 已有 sizeHint 的控件 (JZPushButton) 行为不变 —— 严格取 hint
static bool hint_present_still_wins()
{
    JZWindow win;
    win.resize(400, 300);

    JZWidget* box = new JZWidget(&win);
    JZPushButton* b = new JZPushButton(box);
    b->setText("ok");

    JZHBoxLayout* lay = new JZHBoxLayout(4, 4);
    lay->addWidget(b, 0);
    box->setLayout(lay);
    box->resize(400, 100);
    box->updateLayout();

    JZSize sh = b->sizeHint();
    JZ_TEST_GT(sh.width(), 0);
    JZ_TEST_EQ(sh.width(), b->width());
    return true;
}

// setLayout 后布局成为**控件的子对象** (JZObject 父子树): 所以控件析构会把它一起收掉,
// 布局绝不会活得比容器久 (JZLayout::m_container 不会悬垂)。
static bool layout_is_child_object()
{
    JZWindow win;
    win.resize(400, 300);

    JZWidget* box = new JZWidget(&win);
    JZVBoxLayout* lay = new JZVBoxLayout(4, 4);
    box->setLayout(lay);

    JZ_TEST_EQ(box, lay->parent());
    JZ_TEST_EQ(box->layout(), (JZLayout*)lay);

    bool inChildren = false;
    for (JZObject* c : box->children())
        if (c == lay)
            inChildren = true;
    JZ_TEST_TRUE(inChildren);
    return true;
}

// 布局先被外部销毁 (等价 Qt 的 `delete w->layout()`): ChildRemoved 里把控件的布局指针置空,
// 不留悬垂, 之后还能再设新布局。
static bool layout_destroyed_clears_pointer()
{
    JZWindow win;
    win.resize(400, 300);

    JZWidget* box = new JZWidget(&win);
    JZVBoxLayout* lay = new JZVBoxLayout(4, 4);
    box->setLayout(lay);
    JZ_TEST_EQ(box->layout(), (JZLayout*)lay);

    delete lay;
    JZ_TEST_EQ(box->layout(), (JZLayout*)nullptr);

    box->setLayout(new JZVBoxLayout(4, 4));   // 指针已清 -> 能再设
    JZ_TEST_TRUE(box->layout() != nullptr);
    return true;
}

// 同一份布局换宿主: 旧宿主经 ChildRemoved 自动置空, 新宿主接管。
static bool retarget_layout_clears_old_host()
{
    JZWindow win;
    win.resize(400, 300);

    JZWidget* a = new JZWidget(&win);
    JZWidget* b = new JZWidget(&win);
    JZVBoxLayout* lay = new JZVBoxLayout(4, 4);

    a->setLayout(lay);
    b->setLayout(lay);

    JZ_TEST_EQ(a->layout(), (JZLayout*)nullptr);
    JZ_TEST_EQ(b->layout(), (JZLayout*)lay);
    JZ_TEST_EQ(lay->container(), b);
    JZ_TEST_EQ(lay->parent(), (JZObject*)b);
    return true;
}

// 归 JS 拥有的布局 (setLayoutFromJs) 同样挂成子对象: 控件先析构时随对象树一起回收,
// 底层对象消失后 JS 侧那份守卫句柄自然失效 (终结器不会再 delete 一次)。
static bool js_owned_layout_follows_widget()
{
    JZWindow win;
    win.resize(400, 300);

    JZWidget* box = new JZWidget(&win);
    JZVBoxLayout* lay = new JZVBoxLayout(4, 4);
    JZObjectPointer guard = lay->guardPointer();

    box->setLayoutFromJs(lay);   // 归 JS 拥有: 本控件不显式 delete
    JZ_TEST_EQ(box->layout(), (JZLayout*)lay);

    delete box;                  // 控件先走: 布局随对象树被收掉
    JZ_TEST_TRUE(guard.object() == nullptr);
    return true;
}

static void test_layout(JZTest* test)
{
    auto suite = std::make_shared<JZTestSuite>("JZLayout");
    test->addSuite(suite);
    suite->addTestCase("list_sizehint_sane", list_sizehint_sane);
    suite->addTestCase("list_sizehint_follows_content", list_sizehint_follows_content);
    suite->addTestCase("hbox_fixed_uses_sizehint", hbox_fixed_uses_sizehint);
    suite->addTestCase("vbox_fixed_uses_sizehint", vbox_fixed_uses_sizehint);
    suite->addTestCase("hint_present_still_wins", hint_present_still_wins);
    suite->addTestCase("layout_is_child_object", layout_is_child_object);
    suite->addTestCase("layout_destroyed_clears_pointer", layout_destroyed_clears_pointer);
    suite->addTestCase("retarget_layout_clears_old_host", retarget_layout_clears_old_host);
    suite->addTestCase("js_owned_layout_follows_widget", js_owned_layout_follows_widget);
}

int main(int argc, char* argv[])
{
    JZApplication app;

    JZTest test;
    test_layout(&test);
    return test.run(argc, argv);
}
