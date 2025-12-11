// php_extension/php_bridge.c (编译成 thrift_bridge.so)
#include "php.h"
#include "Zend/zend_globals.h"
#include "Zend/zend_ini.h"
#include "Zend/zend_types.h"
#include "Zend/zend_string.h"
#include "Zend/zend_API.h"
#include "Zend/zend_objects.h"
#include "Zend/zend_exceptions.h"
#include "ext/standard/info.h"
#include "ext/standard/php_string.h"

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


namespace TC {
// --- A. 处理器工厂 (ProcessorFactory) ---
class ProcessorFactory {
private:
    std::map<std::string, std::shared_ptr<apache::thrift::TProcessor>> processors_;
    
public:
    void registerProcessor(const std::string& service_name, std::shared_ptr<apache::thrift::TProcessor> processor) {
        processors_[service_name] = processor;
        std::cout << "[CoreLib] Registered Service: " << service_name << std::endl;
    }

    std::shared_ptr<apache::thrift::TProcessor> getProcessor(const std::string& service_name) {
        auto it = processors_.find(service_name);
        return (it != processors_.end()) ? it->second : nullptr;
    }

    // 静态回调函数，供 C 风格的插件接口调用
    static void staticRegisterCallback(void* factory_instance, const char* service_name, void* t_processor_ptr) {
        ProcessorFactory* factory = static_cast<ProcessorFactory*>(factory_instance);
        // 使用 shared_ptr 包装 TProcessor，保证其生命周期
        std::shared_ptr<apache::thrift::TProcessor> processor((apache::thrift::TProcessor*)t_processor_ptr);
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
static std::vector<void*> plugin_handles;


// --- B. 插件加载器函数 ---
static void load_plugin(const char* plugin_path) {
    void* handle = dlopen(plugin_path, RTLD_LAZY | RTLD_GLOBAL);
    if (!handle) {
        std::cerr << "[CoreLib Error]: Cannot open library " << plugin_path << ": " << dlerror() << std::endl;
        return;
    }
    plugin_handles.push_back(handle);

    RegisterProcessorFunc register_func = (RegisterProcessorFunc)dlsym(handle, PLUGIN_REGISTER_FUNC_NAME);
    if (!register_func) {
        std::cerr << "[CoreLib Error]: Cannot find function " << PLUGIN_REGISTER_FUNC_NAME << " in " << plugin_path << ": " << dlerror() << std::endl;
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

    std::cout << "--- CoreLib Scanning plugin directory: " << dir_path << " ---" << std::endl;

    if ((dp = opendir(dir_path)) == NULL) {
        // 如果目录不存在，创建目录（可选，简化错误处理）
        if (errno == ENOENT) {
            std::cout << "[CoreLib Info]: Plugin directory not found. Skipping scan." << std::endl;
            return;
        }
        std::cerr << "[CoreLib Error]: Could not open directory " << dir_path << ": " << strerror(errno) << std::endl;
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
            std::string full_path = std::string(dir_path) + "/" + d_name;
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

    std::string service_str(service_name, service_len);
    std::shared_ptr<apache::thrift::TProcessor> processor = global_factory.getProcessor(service_str);

    if (!processor) {
        return nullptr;
    }
    
    std::shared_ptr<apache::thrift::transport::TMemoryBuffer> input_transport(new apache::thrift::transport::TMemoryBuffer((uint8_t*)input_buf, input_len));
    std::shared_ptr<apache::thrift::transport::TMemoryBuffer> output_transport(new apache::thrift::transport::TMemoryBuffer());
    
    std::shared_ptr<apache::thrift::protocol::TBinaryProtocol> input_protocol(new apache::thrift::protocol::TBinaryProtocol(input_transport));
    std::shared_ptr<apache::thrift::protocol::TBinaryProtocol> output_protocol(new apache::thrift::protocol::TBinaryProtocol(output_transport));

    try {
        if (!processor->process(input_protocol, output_protocol, nullptr)) {
                return nullptr;
        }
    } catch (const apache::thrift::TException& tx) {
        std::cerr << "[CoreLib Exception]: " << tx.what() << std::endl;
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
  
// --- 类结构体定义 ---
typedef struct _php_thrift_bridge_transport_object {
    // 存储 serviceName (当前调用的目标 Service 名称)
    zend_string *serviceName; 
    
    // 存储 wBuf (写入缓冲区)
    zend_string *wBuf;

    // 存储 rBuf (读取缓冲区)
    zend_string *rBuf;
    
    // 存储 rBufPos (读取缓冲区当前位置)
    zend_long rBufPos;
    
    // Zend 引擎要求必须包含 zend_object
    zend_object std; 
} php_thrift_bridge_transport_object;

zend_class_entry *thrift_bridge_transport_ce;
zend_class_entry *thrift_transport_exception_ce;
static zend_object_handlers thrift_bridge_handlers;

// --- 辅助宏：用于从 zend_object 获取自定义结构体 ---
static zend_always_inline php_thrift_bridge_transport_object *php_thrift_bridge_transport_fetch_object(zend_object *obj) {
    // 通过结构体成员的偏移量计算自定义结构体的起始地址
    return (php_thrift_bridge_transport_object *)((char *)(obj) - XtOffsetOf(php_thrift_bridge_transport_object, std));
}

static void php_thrift_bridge_transport_dtor_object(zend_object *object)
{
    php_thrift_bridge_transport_object *intern = php_thrift_bridge_transport_fetch_object(object);
    
    if (intern->serviceName) {
        zend_string_release(intern->serviceName);
    }
    if (intern->wBuf) {
        zend_string_release(intern->wBuf);
    }
    if (intern->rBuf) {
        zend_string_release(intern->rBuf);
    }
    
    // 调用父类的析构函数
    zend_objects_destroy_object(object);
}

// 对象的创建函数：为 Zend 对象分配自定义结构体
static zend_object *php_thrift_bridge_transport_create_object(zend_class_entry *ce)
{
    php_thrift_bridge_transport_object *intern;

    // 分配内存
    intern = (php_thrift_bridge_transport_object *) 
        emalloc(sizeof(php_thrift_bridge_transport_object) + zend_object_properties_size(ce));
    zend_object_std_init(&intern->std, ce);

    // 【删除原有的两行错误代码】
    // intern->std.handlers 指向我们全局静态的、可写的 handlers
    intern->std.handlers = &thrift_bridge_handlers; // <--- 关键修正
    // 初始化属性
    intern->serviceName = NULL;
    intern->wBuf = NULL;
    intern->rBuf = NULL;
    intern->rBufPos = 0;

    
    return &intern->std;
}

// -----------------------------------------------------
// C 库导出函数
// -----------------------------------------------------
extern "C" {

ZEND_METHOD(ThriftBridgeTransport, __construct)
{
    zend_string *service_name_str;
    
    // 获取当前对象的 C 结构体
    php_thrift_bridge_transport_object *intern = php_thrift_bridge_transport_fetch_object(Z_OBJ_P(getThis()));
    
    // S 表示接收 zend_string
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "S", &service_name_str) == FAILURE) {
        return;
    }

    // 存储 serviceName，使用 zend_string_copy 拷贝字符串
    intern->serviceName = zend_string_copy(service_name_str); 
    
    // 初始化 wBuf/rBuf 为空字符串 (使用常量，不需要释放)
    intern->wBuf = ZSTR_EMPTY_ALLOC();
    intern->rBuf = ZSTR_EMPTY_ALLOC();
    intern->rBufPos = 0;
}

// public function isOpen() { return true; }
ZEND_METHOD(ThriftBridgeTransport, isOpen)
{
    RETURN_TRUE;
}

// public function open() {}
ZEND_METHOD(ThriftBridgeTransport, open)
{
    // NOOP
}

// public function close() {}
ZEND_METHOD(ThriftBridgeTransport, close)
{
    // NOOP
}

// public function write($buf)
ZEND_METHOD(ThriftBridgeTransport, write)
{
    zend_string *buf;
    
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "S", &buf) == FAILURE) {
        return;
    }
    
    php_thrift_bridge_transport_object *intern = php_thrift_bridge_transport_fetch_object(Z_OBJ_P(getThis()));

    // 获取当前 wBuf 的长度和新数据的长度
    size_t old_len = ZSTR_LEN(intern->wBuf);
    size_t write_len = ZSTR_LEN(buf);
    size_t new_len = old_len + write_len;
    
    // 1. 分配新的 zend_string 空间
    // 使用 zend_string_safe_alloc(len_factor, size, extra, persistent)
    zend_string *new_wBuf = zend_string_safe_alloc(1, new_len, 1, 0); 
    
    // 2. 拷贝旧数据 (intern->wBuf)
    memcpy(ZSTR_VAL(new_wBuf), ZSTR_VAL(intern->wBuf), old_len);
    
    // 3. 拷贝新数据 (buf)
    memcpy(ZSTR_VAL(new_wBuf) + old_len, ZSTR_VAL(buf), write_len);
    
    // 4. 设置新字符串的长度和终止符
    ZSTR_LEN(new_wBuf) = new_len;
    ZSTR_VAL(new_wBuf)[new_len] = '\0'; // 必须手动添加终止符
    
    // 5. 释放旧的 wBuf，更新指针
    zend_string_release(intern->wBuf); 
    intern->wBuf = new_wBuf;
}


// public function read($len)
ZEND_METHOD(ThriftBridgeTransport, read)
{
    zend_long len;
    php_thrift_bridge_transport_object *intern;
    
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &len) == FAILURE) {
        return;
    }
    
    intern = php_thrift_bridge_transport_fetch_object(Z_OBJ_P(getThis()));
    
    // 检查 rBuf 是否已关闭或未flush (PHP 版本中是 rBuf === null)
    if (intern->rBuf == NULL) {
        zend_throw_exception_ex(NULL, 0, "Transport is closed or not flushed.");
        return;
    }
    
    size_t total_len = ZSTR_LEN(intern->rBuf);
    
    // 可读取的长度
    size_t read_len = (size_t)len;
    size_t remaining_len = total_len - intern->rBufPos;
    
    if (read_len > remaining_len) {
        read_len = remaining_len;
    }
    
    // 检查是否需要抛出 "No more data to read" 异常
    if (read_len == 0 && len > 0) {
        // 这是 Thrift 抛出 "No more data to read" 异常的条件
        zend_throw_exception_ex(NULL, 0, "Cannot read %ld bytes from transport.", len);
        return;
    }

    // 截取并返回读取的数据
    RETVAL_STRINGL(ZSTR_VAL(intern->rBuf) + intern->rBufPos, read_len);
    
    // 更新读取位置
    intern->rBufPos += read_len;
}

// public function flush()
ZEND_METHOD(ThriftBridgeTransport, flush)
{
    php_thrift_bridge_transport_object *intern = php_thrift_bridge_transport_fetch_object(Z_OBJ_P(getThis()));
    
    // --- 1. 获取请求数据 (intern->wBuf) ---
    const char *requestBinary = ZSTR_VAL(intern->wBuf);
    size_t requestBinaryLen = ZSTR_LEN(intern->wBuf);

    // --- 2. 调用 C++ CoreLib 函数 ---
    size_t responseLen = 0;
    char* responseBinary = process_thrift_data_generic(
        ZSTR_VAL(intern->serviceName), ZSTR_LEN(intern->serviceName),
        requestBinary, requestBinaryLen,
        &responseLen
    );

    // --- 3. 检查 CoreLib 返回结果 ---
    if (responseBinary == NULL) {
        // 抛出 TTransportException
        zend_throw_exception_ex(NULL, 0, "CoreLib RPC failed or returned null.");
        return;
    }

    // --- 4. 将响应数据存入读取缓冲区 ---
    // 释放旧的 rBuf
    zend_string_release(intern->rBuf); 
    
    // 将 C/C++ 返回的 char* 转换为 zend_string (会进行拷贝)
    intern->rBuf = zend_string_init(responseBinary, responseLen, 0); 
    free(responseBinary); // 释放 C++ 层分配的内存 (重要!)
    
    intern->rBufPos = 0;
    
    // --- 5. 清空写入缓冲区 ---
    zend_string_release(intern->wBuf);
    intern->wBuf = ZSTR_EMPTY_ALLOC();
}


const zend_function_entry thrift_bridge_transport_methods[] = {
    ZEND_ME(ThriftBridgeTransport, __construct, NULL, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
    ZEND_ME(ThriftBridgeTransport, isOpen,      NULL, ZEND_ACC_PUBLIC)
    ZEND_ME(ThriftBridgeTransport, open,        NULL, ZEND_ACC_PUBLIC)
    ZEND_ME(ThriftBridgeTransport, close,       NULL, ZEND_ACC_PUBLIC)
    ZEND_ME(ThriftBridgeTransport, read,        NULL, ZEND_ACC_PUBLIC)
    ZEND_ME(ThriftBridgeTransport, write,       NULL, ZEND_ACC_PUBLIC)
    ZEND_ME(ThriftBridgeTransport, flush,       NULL, ZEND_ACC_PUBLIC)
    PHP_FE_END
};


// 类注册函数
static void php_thrift_bridge_transport_init(INIT_FUNC_ARGS)
{
    zend_class_entry ce;
    
    // 假设 TTransport_ce 已经通过外部头文件或扩展加载
    zend_class_entry *parent_ce = NULL; // 替换为 TTransport 的 zend_class_entry

    INIT_CLASS_ENTRY(ce, "ThriftBridgeTransport", thrift_bridge_transport_methods);
    
    // 注册类，并设置继承关系
    thrift_bridge_transport_ce = zend_register_internal_class_ex(&ce, parent_ce);
    
    // 设置对象的创建和销毁处理器
    thrift_bridge_transport_ce->create_object = php_thrift_bridge_transport_create_object;
}


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
    memcpy(&thrift_bridge_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    thrift_bridge_handlers.dtor_obj = php_thrift_bridge_transport_dtor_object;
    php_thrift_bridge_transport_init(type, module_number);
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

// --- PHP MINFO (模块信息) 函数 ---
PHP_MINFO_FUNCTION(thrift_bridge)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "Thrift Dynamic RPC Bridge", "enabled");
    php_info_print_table_row(2, "Version", "1.0");
    php_info_print_table_row(2, "CoreLib Status", "Initialized via RINIT");
    php_info_print_table_end();
}

// --- 扩展模块入口定义 ---
zend_module_entry thrift_bridge_module_entry = {
    STANDARD_MODULE_HEADER,
    "thrift_bridge",        /* 扩展名称 */
    NULL, 
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
