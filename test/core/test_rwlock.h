#pragma once

class JZTest;

// JZReadWriteLock 读写锁测试: 验证读写互斥、多读并发、写优先语义
void test_JZReadWriteLock(JZTest *test);
