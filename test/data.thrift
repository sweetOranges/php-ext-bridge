namespace cpp Dynamic
namespace php DynamicExt

struct InputData {
    1: required i16 transaction_id;
    2: required double amount;
}

struct OutputData {
    1: required i32 result_flag;
    2: required string message;
}

service DynamicServiceA {
    OutputData process_transaction_a(1: InputData input);
}