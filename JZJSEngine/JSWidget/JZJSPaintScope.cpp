#include "JZJSPaintScope.h"
#include "JZJSWidgetModule.h"

JZJSPaintScope::JZJSPaintScope(JZJSWidgetModule* module, JZWidget* widget)
{
	module->beginPainter(widget);
}

JZJSPaintScope::~JZJSPaintScope()
{
	m_module->endPainter();
}