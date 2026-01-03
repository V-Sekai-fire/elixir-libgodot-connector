#ifndef LIBGODOT_H
#define LIBGODOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *GDExtensionObjectPtr;
typedef void *GDExtensionInitializationFunction;
typedef void *InvokeCallbackFunction;
typedef void *ExecutorData;
typedef void *LogCallbackFunction;
typedef void *LogCallbackData;

typedef struct {
    int p_argc;
    char **p_argv;
    GDExtensionInitializationFunction p_init_func;
    InvokeCallbackFunction p_async_func;
    ExecutorData p_async_data;
    InvokeCallbackFunction p_sync_func;
    ExecutorData p_sync_data;
    LogCallbackFunction p_log_func;
    LogCallbackData p_log_data;
} libgodot_create_godot_instance_args;

GDExtensionObjectPtr libgodot_create_godot_instance(
    int p_argc,
    char *p_argv[],
    GDExtensionInitializationFunction p_init_func,
    InvokeCallbackFunction p_async_func,
    ExecutorData p_async_data,
    InvokeCallbackFunction p_sync_func,
    ExecutorData p_sync_data,
    LogCallbackFunction p_log_func,
    LogCallbackData p_log_data
);

void libgodot_destroy_godot_instance(GDExtensionObjectPtr p_instance);

#ifdef __cplusplus
}
#endif

#endif // LIBGODOT_H
