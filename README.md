## common-thrift-c-extension

开发一个php原生扩展，需要掌握php头文件中的很多宏，这无疑成为了一个
门槛。但是开发一个thrift服务是非常简单的。那么有没有一种可能，实现一个
原生c扩展，就像开发一个thrift服务一样简单？答案是可以，这个项目借助thrift
的序列化协议，成功的屏蔽了php的类型，做到了只要实现了thrift的服务就能在php
中扩展c函数的功能。

## 设置

### 在php.ini中设置

```ini
extension=../build/thrift_bridge.so
thrift_bridge.plugin_dir = ./plugins
```

这个扩展会自动加载plugin_dir下所有的thrift服务
### 实现服务
实现一个thrift服务也非常简单

```cpp

// thrift 服务实现
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

// 插件注册入口点实现 ---
extern "C" {
    void register_thrift_processors(ProcessorFactoryContext* context) {
        cout << "  [ServiceA Plugin] Initializing DynamicServiceA..." << endl;
        
        std::shared_ptr<DynamicServiceAHandler> handlerA(new DynamicServiceAHandler());
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
```

编译成so

``` bash
g++ -std=c++11 -fPIC -shared -o ./plugins/libservice_a.so \
./gen-cpp/DynamicServiceA.cpp \
./gen-cpp/data_constants.cpp \
./gen-cpp/data_types.cpp \
./service_a.c \
-I/usr/include  -lthrift
```

### php调用
```php
$serviceName = 'DynamicServiceA';
// 3. 实例化 Client
$client = new DynamicExt\DynamicServiceAClient(new TBinaryProtocolAccelerated(new ThriftBridgeTransport($serviceName))); 
$input_success = ['transaction_id' => 101, 'amount' => 60.00];
$output_success = $client->process_transaction_a(new InputData($input_success));
```
