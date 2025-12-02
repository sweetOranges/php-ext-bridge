#!/bin/bash

CFLAGS=$(php-config --includes)
g++ -std=c++11 -fPIC -shared -o ./build/thrift_bridge.so ./thrift_bridge.c $CFLAGS -L. -lthrift
