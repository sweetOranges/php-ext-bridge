#!/bin/bash

CFLAGS=$(php-config --includes)
g++ -std=c++11 -fPIC -shared -g  $CFLAGS -I./3thrd/include/ -L./3thrd/lib/ -lthrift -Wl,-rpath=/home/stock/workspace/php-ext/test/3thrd/lib \
-o ./build/thrift_bridge.so  ./thrift_bridge.c 