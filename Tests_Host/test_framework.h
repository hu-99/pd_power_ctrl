#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>

extern int g_tests_run;
extern int g_tests_failed;

#define TEST_ASSERT_TRUE(cond)                                                 \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("[FAIL] %s:%d: expected true (%s)\n", __FILE__, __LINE__,   \
                   #cond);                                                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_EQ_INT(expected, actual)                                   \
    do {                                                                       \
        long long exp_val = (long long)(expected);                             \
        long long act_val = (long long)(actual);                               \
        if (exp_val != act_val) {                                              \
            printf("[FAIL] %s:%d: expected %lld, got %lld (%s)\n", __FILE__,   \
                   __LINE__, exp_val, act_val, #actual);                       \
            return 1;                                                          \
        }                                                                      \
    } while (0)

#define RUN_TEST(fn)                                                           \
    do {                                                                       \
        g_tests_run++;                                                         \
        if ((fn)() != 0) {                                                     \
            g_tests_failed++;                                                  \
        } else {                                                               \
            printf("[PASS] %s\n", #fn);                                        \
        }                                                                      \
    } while (0)

#endif /* TEST_FRAMEWORK_H */
