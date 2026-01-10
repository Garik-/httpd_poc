/**
 * @file err.h
 * @brief Error handling macros for ESP-IDF projects
 *
 * Provides macros to check ESP-IDF error codes (`esp_err_t`) and log
 * an error message if the code is not `ESP_OK`. Supports C and C++,
 * including C++20 `__VA_OPT__` for variadic macros.
 *
 * The behavior can be modified by the configuration option
 * CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT.
 *
 * Example usage:
 * @code
 *     ESP_LOG_ON_ERROR(do_something(), TAG, "Failed to execute something");
 * @endcode
 *
 * @author garik.djan <garik.djan@gmail.com>
 * @version 0.0.1
 */

#ifndef _ERR_H_
#define _ERR_H_

#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT)
/**
 * Macro which can be used to check the error code. If the code is not ESP_OK, it prints the message.
 */
#define ESP_LOG_ON_ERROR(x, log_tag, format, ...)                                                                      \
    do {                                                                                                               \
        (x);                                                                                                           \
    } while (0)
#else // !CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT

#if defined(__cplusplus) && (__cplusplus > 201703L)

/**
 * Macro which can be used to check the error code. If the code is not ESP_OK, it prints the message.
 */
#define ESP_LOG_ON_ERROR(x, log_tag, format, ...)                                                                      \
    do {                                                                                                               \
        esp_err_t err_rc_ = (x);                                                                                       \
        if (unlikely(err_rc_ != ESP_OK)) {                                                                             \
            ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__ __VA_OPT__(, ) __VA_ARGS__);                   \
        }                                                                                                              \
    } while (0)
#else // !(defined(__cplusplus) && (__cplusplus >  201703L))

/**
 * Macro which can be used to check the error code. If the code is not ESP_OK, it prints the message.
 */
#define ESP_LOG_ON_ERROR(x, log_tag, format, ...)                                                                      \
    do {                                                                                                               \
        esp_err_t err_rc_ = (x);                                                                                       \
        if (unlikely(err_rc_ != ESP_OK)) {                                                                             \
            ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__);                               \
        }                                                                                                              \
    } while (0)

#endif // !(defined(__cplusplus) && (__cplusplus >  201703L))

#endif // !CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT

#ifdef __cplusplus
}
#endif

#endif // _ERR_H_