#include <iostream>
#include <iomanip>
#include <set>
#include "JZTest.h"
#include "JZStd.h"
#include "JZElapsedTimer.h"
#include "JZCommandLineParser.h"
#include "JZApplication.h"
#include "JZTimer.h"
#include "JZEventLoop.h"

//JZTest
void JZTest::exec(int ms)
{
    JZEventLoop loop;
    JZTimer::singleShot(ms, [&loop] { loop.exit(); });
    loop.exec();
}

JZTest::JZTest() {}

JZTest::~JZTest() {}

void JZTest::addSuite(std::shared_ptr<JZTestSuite> suite) {
    testSuites_.push_back(suite);
}

int JZTest::run(int argc, char** argv)
{
    if (!init(argc, argv))
        return 1;

    JZTestResult ret = runAllSuites();
    if (ret.failed != 0)
        return 1;
    
    return 0;
}

bool JZTest::init(int argc, char** argv) 
{
    JZCommandLineParser parser;
    std::string testArg;

    parser.addOption("--test", &testArg, false);

    if (!parser.parse(argc, argv)) {
        std::cerr << "Error parsing command line: " << parser.errorString() << std::endl;
        return false;
    }

    if (testArg.empty()) {
        return true;
    }

    vector<string> parts = str_split(testArg, ",", JZ::SkipEmptyParts);
    for (const auto& part : parts) {
        string trimmed = str_trimmed(part);
        if (trimmed.empty()) {
            continue;
        }

        size_t dotPos = trimmed.find('.');
        if (dotPos != string::npos) {
            string suiteName = trimmed.substr(0, dotPos);
            string testName = trimmed.substr(dotPos + 1);
            suiteFilters_.push_back(suiteName);
            testFilters_.push_back(testName);
        } else {
            suiteFilters_.push_back(trimmed);
        }
    }

    return true;
}

JZTestResult JZTest::runAllSuites() const
{
    // 构建要运行的测试列表，同时保存 suite 指针
    struct TestItem
    {
        std::shared_ptr<JZTestSuite> suitePtr;
        std::string suiteName;
        std::string testName;
    };
    std::vector<TestItem> testsToRun;

    for (const auto& suite : testSuites_)
    {
        std::string suiteName = suite->name();
        bool shouldRunSuite = suiteFilters_.empty();
        if (!shouldRunSuite)
        {
            for (const auto& filter : suiteFilters_)
            {
                if (suiteName == filter)
                {
                    shouldRunSuite = true;
                    break;
                }
            }
        }
        if (!shouldRunSuite)
        {
            continue;
        }

        auto allTests = suite->testList();

        if (testFilters_.empty())
        {
            // 运行该套件的所有测试
            for (const auto& testName : allTests)
            {
                testsToRun.push_back({suite, suiteName, testName});
            }
        }
        else
        {
            // 只运行指定的测试（按套件和测试名的配对过滤）
            for (size_t i = 0; i < suiteFilters_.size(); ++i)
            {
                if (suiteName == suiteFilters_[i] && i < testFilters_.size())
                {
                    std::string testName = testFilters_[i];
                    if (suite->hasTest(testName))
                    {
                        testsToRun.push_back({suite, suiteName, testName});
                    }
                }
            }
        }
    }

    int totalTests = static_cast<int>(testsToRun.size());

    // 统计实际参与的套件数（去重）
    std::set<std::string> uniqueSuites;
    for (const auto& item : testsToRun)
    {
        uniqueSuites.insert(item.suiteName);
    }
    int totalSuitesRun = static_cast<int>(uniqueSuites.size());

    // 2. 现在打印标题（使用实际数据）
    std::cout << "====================================" << std::endl;
    std::cout << "JZTest Framework" << std::endl;
    std::cout << "Total test suites: " << testSuites_.size()  // 全部套件数
              << " (running " << totalSuitesRun << ")" << std::endl;
    std::cout << "Total tests to run: " << totalTests << std::endl;
    std::cout << "====================================" << std::endl;
    std::cout << std::endl;

    int total = static_cast<int>(testsToRun.size());
    int passed = 0;
    int failed = 0;

    // 运行测试并打印结果（带执行时间）
    for (int i = 0; i < total; ++i)
    {
        const auto& item = testsToRun[i];
        std::string displayName = item.suiteName + "." + item.testName;

        // 计时并运行
        JZElapsedTimer timer;
        timer.start();
        bool result = item.suitePtr->runTest(item.testName);
        long long elapsedMs = timer.elapsed();  // 假设返回毫秒数

        if (result)
            ++passed;
        else
            ++failed;

        // 结果行（包含耗时）
        std::cout << std::setw(2) << (i + 1) << "/" << total << " Test #" << std::setw(2) << (i + 1) << ": "
                  << displayName << " ................   " << (result ? "Passed" : "Failed") << "   " << std::fixed
                  << std::setprecision(3) << (elapsedMs / 1000.0) << " sec" << std::endl;
    }

    // 最终统计
    std::cout << std::endl;
    double passRate = total > 0 ? (static_cast<double>(passed) / total * 100.0) : 0.0;
    std::cout << std::fixed << std::setprecision(0) << passRate << "% tests passed, " << failed
              << " tests failed out of " << total << std::endl;

    std::cout << "====================================" << std::endl;
    std::cout << "All test suites completed" << std::endl;
    std::cout << "====================================" << std::endl;

    JZTestResult ret;
    ret.passed = passed;
    ret.failed = failed;
    return ret;
}
