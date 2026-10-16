#pragma once
#include "esp_err.h"
typedef int (*esp_console_cmd_func_with_context_t)(void*, int, char**);
typedef int (*esp_console_cmd_func_t)(int, char**);
struct esp_console_cmd_t { const char* command; const char* help; const char* hint; esp_console_cmd_func_t func; void* argtable; esp_console_cmd_func_with_context_t func_w_context; void* context; };
struct esp_console_repl_config_t { const char* prompt; size_t max_cmdline_length; unsigned task_stack_size; };
struct esp_console_dev_uart_config_t { int channel; };
struct esp_console_repl_t;
#define ESP_CONSOLE_REPL_CONFIG_DEFAULT() esp_console_repl_config_t{}
#define ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT() esp_console_dev_uart_config_t{}
inline esp_err_t esp_console_new_repl_uart(const esp_console_dev_uart_config_t*, const esp_console_repl_config_t*, esp_console_repl_t**) { return ESP_OK; }
inline esp_err_t esp_console_cmd_register(const esp_console_cmd_t*) { return ESP_OK; }
inline esp_err_t esp_console_start_repl(esp_console_repl_t*) { return ESP_OK; }
#define ESP_ERROR_CHECK(x) (void)(x)
