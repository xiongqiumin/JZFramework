#ifndef JZTEST_H
#define JZTEST_H
#include "JZTestMacro.h"
#include "JZTestSuite.h"
#include "JZBenchmark.h"
#include <memory>
#include <vector>
#include <string>

struct JZFRAMEWORK_API JZTestResult
{
    int passed = 0;
    int failed = 0;
};

class JZFRAMEWORK_API JZTest 
{
public:
    static void exec(int ms);

    JZTest();
    ~JZTest();
    
    void addSuite(std::shared_ptr<JZTestSuite> suite);
    int run(int argc, char** argv);

private:
    bool init(int argc, char** argv);
    JZTestResult runAllSuites() const;

    std::vector<std::shared_ptr<JZTestSuite>> testSuites_;
    std::vector<std::string> testFilters_;
    std::vector<std::string> suiteFilters_;
};

#endif
