#ifndef JZ_JS_PAINT_SCOPE_H_
#define JZ_JS_PAINT_SCOPE_H_

#include <vector>

#include "quickjs/quickjs.h"
#include "JZJSType.h"
#include "JZPainter.h"

class JZJSWidgetModule;
class JZJSPaintScope
{
public:
    JZJSPaintScope(JZJSWidgetModule *module, JZWidget *widget);
    ~JZJSPaintScope();

    JZJSWidgetModule* m_module = nullptr;
};

#endif // !JZ_JS_PAINT_SCOPE_H_
