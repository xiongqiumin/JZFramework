#include "JZScopeGuard.h"

JZScopeGuard::JZScopeGuard(std::function<void()> func) : m_func(std::move(func)), m_active(true) {}

JZScopeGuard::~JZScopeGuard() {
    if (m_active && m_func) {
        m_func();
    }
}

void JZScopeGuard::dismiss()
{
    m_active = false;
}