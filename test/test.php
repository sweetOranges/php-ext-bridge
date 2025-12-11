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
use DynamicExt\InputData;

// 确保我们的 PHP 扩展已加载
if (!extension_loaded('thrift_bridge')) {
    die("Error: PHP extension 'thrift_bridge' is not loaded. Please check your php.ini.\n");
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