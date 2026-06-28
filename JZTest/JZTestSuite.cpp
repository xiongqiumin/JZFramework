#include "JZTestSuite.h"
#include <iostream>

JZTestSuite::JZTestSuite(const std::string& name) : suiteName_(name) {}

JZTestSuite::~JZTestSuite() {}

std::vector<std::string> JZTestSuite::testList() const
{
    std::vector<std::string> ret;
    for (auto& item : testCases_)
        ret.push_back(item.name);

    return ret;
}


void JZTestSuite::addTestCase(const std::string& name, std::function<bool()> testCase)
{
    testCases_.push_back({name, testCase});
}

bool JZTestSuite::runTest(const std::string& testName) const 
{
    bool ret = false;

    for (const auto& testCase : testCases_) {
        if (testCase.name != testName) {
            continue;
        }

        try {
            if (testCase.testFunc())
            {
                ret = true;
            }
        } catch (const std::exception& e) {
            std::cout << "Exception" << std::endl;
            std::cout << "  Error: " << e.what() << std::endl;
        }
    }

    return ret;
}

bool JZTestSuite::hasTest(const std::string& testName) const 
{
    for (const auto& testCase : testCases_) {
        if (testCase.name == testName) {
            return true;
        }
    }
    return false;
}

std::string JZTestSuite::name() const 
{
    return suiteName_;
}
