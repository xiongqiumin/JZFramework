#ifndef JZTESTASSERT_H
#define JZTESTASSERT_H

#include <sstream>
#include <stdexcept>
#include <string>

namespace JZTestInternal {
    template<typename T>
    std::string toString(const T& value) {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }
    
    template<>
    std::string toString(const std::nullptr_t&);

    inline void throwAssertionFailure(const std::string& message, const char* file, int line) {
        std::ostringstream oss;
        oss << "Assertion failed at " << file << ":" << line << " - " << message;
        throw std::runtime_error(oss.str());
    }
}

#define JZ_TEST_ASSERT(condition) \
    do { \
        if (!(condition)) { \
            JZTestInternal::throwAssertionFailure( \
                "Assertion failed: " #condition, __FILE__, __LINE__); \
        } \
    } while(0)

#define JZ_TEST_EQ(expected, actual) \
    do { \
        auto _jz_expected = (expected); \
        auto _jz_actual = (actual); \
        if (_jz_expected != _jz_actual) { \
            JZTestInternal::throwAssertionFailure( \
                "Expected " + JZTestInternal::toString(_jz_expected) + \
                " but got " + JZTestInternal::toString(_jz_actual), \
                __FILE__, __LINE__); \
        } \
    } while(0)

#define JZ_TEST_NEQ(not_expected, actual) \
    do { \
        auto _jz_not_expected = (not_expected); \
        auto _jz_actual = (actual); \
        if (_jz_not_expected == _jz_actual) { \
            JZTestInternal::throwAssertionFailure( \
                "Expected not equal to " + JZTestInternal::toString(_jz_not_expected) + \
                " but got " + JZTestInternal::toString(_jz_actual), \
                __FILE__, __LINE__); \
        } \
    } while(0)

#define JZ_TEST_TRUE(value) \
    do { \
        auto _jz_value = (value); \
        if (!_jz_value) { \
            JZTestInternal::throwAssertionFailure( \
                "Expected true but got " + JZTestInternal::toString(_jz_value), \
                __FILE__, __LINE__); \
        } \
    } while(0)

#define JZ_TEST_FALSE(value) \
    do { \
        auto _jz_value = (value); \
        if (_jz_value) { \
            JZTestInternal::throwAssertionFailure( \
                "Expected false but got " + JZTestInternal::toString(_jz_value), \
                __FILE__, __LINE__); \
        } \
    } while(0)

#define JZ_TEST_GT(expected, actual) \
    do { \
        auto _jz_expected = (expected); \
        auto _jz_actual = (actual); \
        if (!(_jz_actual > _jz_expected)) { \
            JZTestInternal::throwAssertionFailure( \
                "Expected " + JZTestInternal::toString(_jz_actual) + \
                " > " + JZTestInternal::toString(_jz_expected), \
                __FILE__, __LINE__); \
        } \
    } while(0)

#define JZ_TEST_GE(expected, actual) \
    do { \
        auto _jz_expected = (expected); \
        auto _jz_actual = (actual); \
        if (!(_jz_actual >= _jz_expected)) { \
            JZTestInternal::throwAssertionFailure( \
                "Expected " + JZTestInternal::toString(_jz_actual) + \
                " >= " + JZTestInternal::toString(_jz_expected), \
                __FILE__, __LINE__); \
        } \
    } while(0)

#define JZ_TEST_LT(expected, actual) \
    do { \
        auto _jz_expected = (expected); \
        auto _jz_actual = (actual); \
        if (!(_jz_actual < _jz_expected)) { \
            JZTestInternal::throwAssertionFailure( \
                "Expected " + JZTestInternal::toString(_jz_actual) + \
                " < " + JZTestInternal::toString(_jz_expected), \
                __FILE__, __LINE__); \
        } \
    } while(0)

#define JZ_TEST_LE(expected, actual) \
    do { \
        auto _jz_expected = (expected); \
        auto _jz_actual = (actual); \
        if (!(_jz_actual <= _jz_expected)) { \
            JZTestInternal::throwAssertionFailure( \
                "Expected " + JZTestInternal::toString(_jz_actual) + \
                " <= " + JZTestInternal::toString(_jz_expected), \
                __FILE__, __LINE__); \
        } \
    } while(0)

#define JZ_TEST_THROW(statement, exception_type) \
    do { \
        bool _jz_thrown = false; \
        try { \
            statement; \
        } catch (const exception_type&) { \
            _jz_thrown = true; \
        } catch (...) { \
            JZTestInternal::throwAssertionFailure( \
                "Expected " #exception_type " to be thrown, but different exception was thrown", \
                __FILE__, __LINE__); \
        } \
        if (!_jz_thrown) { \
            JZTestInternal::throwAssertionFailure( \
                "Expected " #exception_type " to be thrown, but no exception was thrown", \
                __FILE__, __LINE__); \
        } \
    } while(0)

#define JZ_TEST_NO_THROW(statement) \
    do { \
        try { \
            statement; \
        } catch (const std::exception& e) { \
            JZTestInternal::throwAssertionFailure( \
                "Expected no exception, but " + std::string(e.what()) + " was thrown", \
                __FILE__, __LINE__); \
        } catch (...) { \
            JZTestInternal::throwAssertionFailure( \
                "Expected no exception, but unknown exception was thrown", \
                __FILE__, __LINE__); \
        } \
    } while(0)

#endif
