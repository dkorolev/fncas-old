#!/bin/bash
make autogen/functions.h
g++ --std=c++0x -O3 -DFNCAS_JIT=NASM crash_test.cc -o crash -ldl
valgrind --leak-check=yes ./crash
