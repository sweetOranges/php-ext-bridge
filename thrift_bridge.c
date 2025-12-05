// php_extension/php_bridge.c (编译成 thrift_bridge.so)

#include "php.h"
#include "Zend/zend_ini.h"
#include "ext/standard/info.h"
#include <stdlib.h> 
#include <string.h>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <dlfcn.h> 
#include <sys/types.h>
#include <dirent.h>
#include <errno.h> // for strerror

// Thrift 真实头文件
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/transport/TBufferTransports.h>
#include <thrift/TProcessor.h>

#include "./plugin_api.h"
#define PLUGIN_SUFFIX ".so"

using namespace apache::thrift;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;
using namespace std;
namespace TC {
// --- A. 处理器工厂 (ProcessorFactory) ---
class ProcessorFactory {
private:
    map<string, shared_ptr<TProcessor>> processors_;
    
public:
    void registerProcessor(const string& service_name,shared_ptr<TProcessor> processor) {
        processors_[service_name] = processor;
        cout << "[CoreLib] Registered Service: " << service_name << endl;
    }

   shared_ptr<TProcessor> getProcessor(const string& service_name) {
        auto it = processors_.find(service_name);
        return (it != processors_.end()) ? it->second : nullptr;
    }

    // 静态回调函数，供 C 风格的插件接口调用
    static void staticRegisterCallback(void* factory_instance, const char* service_name, void* t_processor_ptr) {
        ProcessorFactory* factory = static_cast<ProcessorFactory*>(factory_instance);
        // 使用 shared_ptr 包装 TProcessor，保证其生命周期
       shared_ptr<TProcessor> processor((TProcessor*)t_processor_ptr);
        factory->registerProcessor(service_name, processor);
    }
    void clean()
    {
        processors_.clear();
    }
};


}

static TC::ProcessorFactory global_factory; 
static bool core_initialized = false;
static vector<void*> plugin_handles;

// --- B. 插件加载器函数 ---
static void load_plugin(const char* plugin_path) {
    void* handle = dlopen(plugin_path, RTLD_LAZY | RTLD_GLOBAL);
    if (!handle) {
        cerr << "[CoreLib Error]: Cannot open library " << plugin_path << ": " << dlerror() << endl;
        return;
    }
    plugin_handles.push_back(handle);

    RegisterProcessorFunc register_func = (RegisterProcessorFunc)dlsym(handle, PLUGIN_REGISTER_FUNC_NAME);
    if (!register_func) {
        cerr << "[CoreLib Error]: Cannot find function " << PLUGIN_REGISTER_FUNC_NAME << " in " << plugin_path << ": " << dlerror() << endl;
        return;
    }

    // 构建上下文结构体
    ProcessorFactoryContext context;
    context.factory_instance = &global_factory;
    context.register_func_ptr = TC::ProcessorFactory::staticRegisterCallback;
    
    register_func(&context);
}

// --- C. 自动扫描目录 ---
static void load_plugins_from_directory(const char* dir_path) {
    DIR *dp;
    struct dirent *dirp;

    cout << "--- CoreLib Scanning plugin directory: " << dir_path << " ---" << endl;

    if ((dp = opendir(dir_path)) == NULL) {
        // 如果目录不存在，创建目录（可选，简化错误处理）
        if (errno == ENOENT) {
            cout << "[CoreLib Info]: Plugin directory not found. Skipping scan." << endl;
            return;
        }
        cerr << "[CoreLib Error]: Could not open directory " << dir_path << ": " << strerror(errno) << endl;
        return;
    }

    while ((dirp = readdir(dp)) != NULL) {
        const char* d_name = dirp->d_name;
        size_t name_len = strlen(d_name);
        size_t suffix_len = strlen(PLUGIN_SUFFIX);

        if (strcmp(d_name, ".") == 0 || strcmp(d_name, "..") == 0) {
            continue;
        }

        if (name_len > suffix_len && 
            strcmp(d_name + name_len - suffix_len, PLUGIN_SUFFIX) == 0) 
        {
            string full_path = string(dir_path) + "/" + d_name;
            load_plugin(full_path.c_str());
        }
    }
    closedir(dp);
}

static void initialize_core_lib(const char* plugin_dir) {
    if (core_initialized) return;

    // 调用自动扫描，使用 INI 配置的路径
    load_plugins_from_directory(plugin_dir); 
    
    core_initialized = true;
}
    
static char* process_thrift_data_generic(
    const char* service_name, size_t service_len,
    const char* input_buf, size_t input_len, 
    size_t* output_len) 
{
    // 核心 RPC 逻辑 (与前例相同)
    if (!core_initialized) return nullptr;

    string service_str(service_name, service_len);
    shared_ptr<TProcessor> processor = global_factory.getProcessor(service_str);

    if (!processor) {
        return nullptr;
    }
    
    shared_ptr<TMemoryBuffer> input_transport(new TMemoryBuffer((uint8_t*)input_buf, input_len));
    shared_ptr<TMemoryBuffer> output_transport(new TMemoryBuffer());
    
    shared_ptr<TBinaryProtocol> input_protocol(new TBinaryProtocol(input_transport));
    shared_ptr<TBinaryProtocol> output_protocol(new TBinaryProtocol(output_transport));

    try {
        if (!processor->process(input_protocol, output_protocol, nullptr)) {
                return nullptr;
        }
    } catch (const TException& tx) {
        cerr << "[CoreLib Exception]: " << tx.what() << endl;
        return nullptr;
    }
    
    uint8_t* buf;
    uint32_t len;
    output_transport->getBuffer(&buf, &len);
    
    char* result = (char*)malloc(len);
    if (result == nullptr) return nullptr;
    memcpy(result, buf, len);
    *output_len = len;
    
    return result; 
}


// -----------------------------------------------------
// C 库导出函数
// -----------------------------------------------------
extern "C" {
ZEND_BEGIN_MODULE_GLOBALS(thrift_bridge)
    char *plugin_dir;
ZEND_END_MODULE_GLOBALS(thrift_bridge)
ZEND_DECLARE_MODULE_GLOBALS(thrift_bridge)
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("thrift_bridge.plugin_dir", "./plugins", PHP_INI_ALL, OnUpdateString, plugin_dir, zend_thrift_bridge_globals, thrift_bridge_globals)
PHP_INI_END()
#define THRIFT_BRIDGE_G(v) (thrift_bridge_globals.v)
static void php_thrift_bridge_init_globals(zend_thrift_bridge_globals *globals)
{
    // globals->plugin_dir = NULL;
}

// --- PHP 函数声明 ---
PHP_FUNCTION(call_thrift_processor_generic);
PHP_RINIT_FUNCTION(thrift_bridge);
PHP_MINIT_FUNCTION(thrift_bridge);
PHP_MINFO_FUNCTION(thrift_bridge);
PHP_MSHUTDOWN_FUNCTION(thrift_bridge);

// --- 模块初始化函数 (MINIT) ---
PHP_MINIT_FUNCTION(thrift_bridge)
{
    ZEND_INIT_MODULE_GLOBALS(thrift_bridge, php_thrift_bridge_init_globals, NULL);
    REGISTER_INI_ENTRIES();
    return SUCCESS;
}

// --- 模块关闭函数 (MSHUTDOWN) ---
PHP_MSHUTDOWN_FUNCTION(thrift_bridge)
{
    UNREGISTER_INI_ENTRIES(); 
    global_factory.clean();   
    // 释放所有插件句柄 (防止内存泄漏，虽然在 MSHUTDOWN 时 PHP 进程可能即将退出)
    for (void* handle : plugin_handles) {
        dlclose(handle);
    }
    plugin_handles.clear();
    // 注销 INI 配置
    return SUCCESS;
}

PHP_RINIT_FUNCTION(thrift_bridge)
{
    const char *plugin_path = THRIFT_BRIDGE_G(plugin_dir);
    fprintf(stderr, "[DEBUG] RINIT Plugin Directory: %s\n", plugin_path ? plugin_path : "NULL");
    if (plugin_path == NULL) {
        plugin_path = "./plugins";
    }
    // 传递配置值给 C++ 核心库进行初始化
    initialize_core_lib(plugin_path);
    
    return SUCCESS;
}

// --- PHP 函数实现 (通用 RPC 调用) ---
PHP_FUNCTION(call_thrift_processor_generic)
{
    char *service_name = NULL;
    size_t service_len;
    char *input_buf = NULL;
    size_t input_len;
    
    // 1. 解析两个字符串参数: ServiceName 和 BinaryData ("ss")
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", 
                              &service_name, &service_len,
                              &input_buf, &input_len) == FAILURE) 
    {
        return; // zend_parse_parameters 失败会设置错误状态
    }

    size_t output_len = 0;
    char* output_buf = NULL;
    
    // 2. 调用 C++ 通用处理器
    output_buf = process_thrift_data_generic(
        service_name, service_len,
        input_buf, input_len, 
        &output_len
    );

    if (output_buf == NULL) {
        // 如果返回 NULL，说明 C++ 侧处理失败或 Service 未注册
        php_error_docref(NULL, E_WARNING, "RPC failed, check CoreLib output for errors. Service: %.*s", 
                         (int)service_len, service_name);
        RETURN_FALSE;
    }

    // 3. 返回结果并释放内存
    // RETURN_STRINGL 会复制 output_buf 中的数据到 PHP 内存
    RETURN_STRINGL(output_buf, output_len); 
    
    // 4. 释放 C++ 核心库中 malloc 的内存
    free(output_buf); 
}

// --- PHP MINFO (模块信息) 函数 ---
PHP_MINFO_FUNCTION(thrift_bridge)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "Thrift Dynamic RPC Bridge", "enabled");
    php_info_print_table_row(2, "Version", "1.0");
    php_info_print_table_row(2, "CoreLib Status", "Initialized via RINIT");
    php_info_print_table_end();
}

// --- 扩展函数列表 ---
const zend_function_entry thrift_bridge_functions[] = {
    PHP_FE(call_thrift_processor_generic, NULL) 
    PHP_FE_END
};

// --- 扩展模块入口定义 ---
zend_module_entry thrift_bridge_module_entry = {
    STANDARD_MODULE_HEADER,
    "thrift_bridge",        /* 扩展名称 */
    thrift_bridge_functions, 
    PHP_MINIT(thrift_bridge),                   /* MINT (模块初始化) */
    PHP_MSHUTDOWN(thrift_bridge),                   /* MSHUTDOWN (模块关闭) */
    PHP_RINIT(thrift_bridge), /* RINIT (请求初始化) */
    NULL,                   /* RSHUTDOWN (请求关闭) */
    PHP_MINFO(thrift_bridge), /* MINFO (模块信息) */
    "1.0",                  /* 扩展版本 */
    STANDARD_MODULE_PROPERTIES
};

// 必须添加这个宏，以使 PHP 知道如何加载这个模块
// #ifdef COMPILE_DL_THRIFT_BRIDGE
// #ifdef ZTS
// ZEND_TSRMLS_CACHE_DEFINE()
// #endif
ZEND_GET_MODULE(thrift_bridge)
// #endif

}
