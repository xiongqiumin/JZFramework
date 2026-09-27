#ifndef JZTESTSUITE_H
#define JZTESTSUITE_H
#include "JZGlobal.h"
#include <string>
#include <functional>
#include <vector>
#include <memory>

class JZFRAMEWORK_API JZTestSuite {
public:
    JZTestSuite(const std::string& name);
    ~JZTestSuite();

    std::string name() const;
    std::vector<std::string> testList() const;
    void addTestCase(const std::string& name, std::function<bool()> testCase);
    bool runTest(const std::string& testName) const;
    bool hasTest(const std::string& testName) const;

private:
    struct TestCase {
        std::string name;
        std::function<bool()> testFunc;
    };

    std::string suiteName_;
    std::vector<TestCase> testCases_;
};

#endif
