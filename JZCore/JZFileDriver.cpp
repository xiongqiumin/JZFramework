#include "JZFileDriver.h"

JZFileAbstractDriver *JZFileAbstractDriver::s_instance = nullptr;

JZFileAbstractDriver *JZFileAbstractDriver::instance() { return s_instance; }
void JZFileAbstractDriver::setInstance(JZFileAbstractDriver *inst) { s_instance = inst; }
