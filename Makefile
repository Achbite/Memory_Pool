# Makefile for Memory Pool Project

# 编译器 - 自动检测或使用环境变量
ifdef CXX
    # 使用用户设置的CXX环境变量
else ifneq (,$(wildcard /mingw64/bin/g++.exe))
    CXX = /mingw64/bin/g++.exe
else ifneq (,$(wildcard C:/msys64/mingw64/bin/g++.exe))
    CXX = C:/msys64/mingw64/bin/g++.exe
else ifneq (,$(wildcard D:/MSYS2/mingw64/bin/g++.exe))
    CXX = D:/MSYS2/mingw64/bin/g++.exe
else ifneq (,$(shell which g++ 2>/dev/null))
    CXX = $(shell which g++)
else
    $(error No C++ compiler found! Please install MinGW-w64 or set CXX environment variable)
endif

# 编译选项
# -std=c++11: 使用 C++11 标准 (兼容旧版 GCC)
# -Iinclude: 添加头文件搜索路径
# -O2: 开启二级优化
# -Wall: 开启所有警告
CXXFLAGS = -std=c++11 -Iinclude -O2 -Wall

# 目标可执行文件名称
TARGET = MemoryPool.exe

# 源文件列表
SRCS = src/main.cpp

# 默认构建目标
all: $(TARGET)

# 构建规则
$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS)

# 清理规则 (适配 Windows CMD)
clean:
	@if exist $(TARGET) del $(TARGET)
