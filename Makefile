# Makefile for Memory Pool Project

# 编译器
# 使用用户指定的绝对路径
CXX = "D:/Dev-Cpp/MinGW64/bin/g++.exe"

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
