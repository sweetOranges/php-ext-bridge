<?php
// test.php

/**
 * 确保 PHP 能够找到 Thrift 运行时库和我们生成的类。
 * 在实际部署中，通常通过 Composer 的 autoloading 或手动 require 实现。
 */
require_once __DIR__ . '/gen-php/DynamicExt/Types.php';
require_once __DIR__ . '/gen-php/DynamicExt/DynamicServiceA.php';

require_once __DIR__ . '/vendor/autoload.php';

// --- 引入 Thrift 核心组件和生成的类 ---
use \Thrift\Protocol\TBinaryProtocolAccelerated;
use \Thrift\Transport\TTransport;

// 引入生成的 Service Client 和 Data Structures
use DynamicExt\DynamicServiceAClient;
use DynamicExt\InputData;

// 确保我们的 PHP 扩展已加载
if (!extension_loaded('thrift_bridge')) {
    die("Error: PHP extension 'thrift_bridge' is not loaded. Please check your php.ini.\n");
}

/**
 * 实现一个自定义 Transport，用于桥接 PHP 扩展函数 call_thrift_processor_generic。
 */
class ThriftBridgeTransport extends TTransport
{
    private $serviceName; // 当前调用的目标 Service 名称
    private $wBuf = '';   // 写入缓冲区 (接收 Client 序列化的请求)
    private $rBuf = '';   // 读取缓冲区 (存储 CoreLib 返回的响应)
    private $rBufPos = 0; // 读取缓冲区当前位置

    public function __construct(string $serviceName)
    {
        $this->serviceName = $serviceName;
    }

    public function isOpen() { return true; }
    public function open() {}
    public function close() {}

    /**
     * TClient 调用 write() 来序列化请求数据。
     */
    public function write($buf)
    {
        $this->wBuf .= $buf;
    }

    /**
     * TClient 调用 read() 来读取 CoreLib 返回的响应数据。
     */
    public function read($len)
    {
        if ($this->rBuf === null) {
             throw new TTransportException('Transport is closed or not flushed.', TTransportException::UNKNOWN);
        }
        
        $read = '';
        $readLen = min($len, strlen($this->rBuf) - $this->rBufPos);
        
        if ($readLen > 0) {
            $read = substr($this->rBuf, $this->rBufPos, $readLen);
            $this->rBufPos += $readLen;
        }

        if (strlen($read) != $len && $readLen == 0) {
             // 这是 Thrift 抛出 "No more data to read" 异常的条件
             throw new TTransportException('Cannot read ' . $len . ' bytes from transport.', TTransportException::UNKNOWN);
        }
        
        return $read;
    }

    /**
     * Client 调用 flush() 时，我们执行核心的 C 扩展调用。
     */
    public function flush()
    {
        if (!function_exists('call_thrift_processor_generic')) {
            throw new TTransportException('PHP extension function call_thrift_processor_generic is not available.', TTransportException::UNKNOWN);
        }

        // 1. 获取序列化后的 RPC 请求 (wBuf)
        $requestBinary = $this->wBuf;
        
        // 2. 调用 C 扩展函数
        $responseBinary = call_thrift_processor_generic($this->serviceName, $requestBinary);

        // 3. 检查 CoreLib 返回结果
        if ($responseBinary === false || $responseBinary === null) {
            throw new TTransportException('CoreLib RPC failed or returned null.', TTransportException::UNKNOWN);
        }

        // 4. 将响应数据存入读取缓冲区
        $this->rBuf = $responseBinary;
        $this->rBufPos = 0;
        
        // 5. 清空写入缓冲区 (为下次调用做准备)
        $this->wBuf = '';
    }
}

// ----------------------------------------------------
// --- 实际测试：与标准 Thrift 调用一致 ---
// ----------------------------------------------------

try {
    $serviceName = 'DynamicServiceA';
    // 3. 实例化 Client
    $client = new DynamicExt\DynamicServiceAClient(new TBinaryProtocolAccelerated(new ThriftBridgeTransport($serviceName))); 


    $input_success = ['transaction_id' => 101, 'amount' => 60.00];
    $output_success = $client->process_transaction_a(new InputData($input_success));

    echo "Status: " . ($output_success->result_flag ? 'OK' : 'FAIL') . "\n";
    echo "Message: " . $output_success->message . "\n";

    echo "\n----------------------------------------------------\n";

    // TEST 2: 失败调用 (CoreLib 逻辑失败)
    $input_fail = ['transaction_id' => 102, 'amount' => 150.00];
    $output_fail = $client->process_transaction_a(new InputData($input_fail));

    echo "Status: " . ($output_fail->result_flag ? 'OK' : 'FAIL') . "\n";
    echo "Message: " . $output_fail->message . "\n";

    echo "\n----------------------------------------------------\n";
    
} catch (\Exception $e) {
    echo "An exception occurred during RPC: " . $e->getMessage() . "\n";
    // 捕获 TTransportException 或其他 Thrift/PHP 异常
}