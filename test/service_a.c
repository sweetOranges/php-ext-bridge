// libservice_a/libservice_a.cpp (编译成 libservice_a.so)

#include <iostream>
#include <memory>
#include <string>

// Thrift 真实头文件
#include <thrift/TProcessor.h>

#include "../plugin_api.h"
#include "./gen-cpp/DynamicServiceA.h" // 假设已由 Thrift 编译生成

using namespace Dynamic;
using namespace apache::thrift;
using namespace std;

// --- A. 业务 Handler 实现 ---
class DynamicServiceAHandler : public DynamicServiceAIf {
public:
    void process_transaction_a(OutputData& _return, const InputData& input) override {
        if (input.amount > 100.0) {
            _return.result_flag = 0; 
            _return.message = "ServiceA: Transaction denied.";
        } else {
            _return.result_flag = 1; 
            _return.message = "ServiceA: ID " + to_string(input.transaction_id) + " processed.";
        }
    }
};

// --- B. 插件注册入口点实现 ---
extern "C" {
    void register_thrift_processors(ProcessorFactoryContext* context) {
        cout << "  [ServiceA Plugin] Initializing DynamicServiceA..." << endl;
        
        boost::shared_ptr<DynamicServiceAHandler> handlerA(new DynamicServiceAHandler());
        // TProcessor* 裸指针
        TProcessor* processorA = new DynamicServiceAProcessor(handlerA); 

        // 注册到核心库的工厂中
        context->register_func_ptr(
            context->factory_instance,
            "DynamicServiceA", 
            (void*)processorA
        );
    }
}