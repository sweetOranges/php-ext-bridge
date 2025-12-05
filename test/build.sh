#!/bin/bash

g++ -std=c++11 -fPIC -shared -o ./plugins/libservice_a.so \
./gen-cpp/DynamicServiceA.cpp \
./gen-cpp/data_types.cpp \
./service_a.c \
-I../3thrd/include -L../3thrd/lib/ -lthrift