// common/plugin_api.h
#ifndef PLUGIN_API_H
#define PLUGIN_API_H

#include <stddef.h>

// 宏定义插件注册函数的名称
#define PLUGIN_REGISTER_FUNC_NAME "register_thrift_processors"

// 前向声明 ProcessorFactory 结构体（用于 C 接口）
struct ProcessorFactoryContext;

// 定义插件注册函数签名：所有插件 .so 必须实现这个函数
typedef void (*RegisterProcessorFunc)(struct ProcessorFactoryContext* context);

// 约定用于演示的简化版 ProcessorFactory 接口 (实际中需要提供 TProcessor 接口)
struct ProcessorFactoryContext {
    // 注册 TProcessor 的函数指针：
    // void (*registerProcessor)(void* self, const char* service_name, apache::thrift::TProcessor* processor);
    // 这里的 void* 是指向 ProcessorFactory 实例的指针
    void* factory_instance;
    // 实际的注册函数指针，用于注册 TProcessor
    void (*register_func_ptr)(void* factory_instance, const char* service_name, void* t_processor_ptr);
};

#endif // PLUGIN_API_H