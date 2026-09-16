---
name: JZFramework 使用指南
description: 帮助开发者理解和使用 JZFramework C++ 框架的技能
---

# JZFramework 使用指南

## 描述

JZFramework 是一个跨平台的 C++ 应用框架，提供了类似 Qt 的编程模型，包括信号槽机制、事件循环、网络通信、协程等核心功能。

## 使用场景

- 当用户需要开发基于 JZFramework 的应用程序时
- 当用户询问 JZFramework 的模块、API 或使用方法时
- 当用户需要集成 JZFramework 到 CMake 项目时
- 当用户需要实现信号槽、线程、协程等功能时

## 指令

### 一、框架概述

JZFramework 主要模块：

| 模块 | 功能描述 | 依赖模块 |
|------|----------|----------|
| **JZCore** | 核心模块，提供基础类和工具 | 无 |
| **JZNet** | 网络通信模块（HTTP、WebSocket、TCP） | JZCore |
| **JZCoroutine** | 协程模块，支持异步编程 | JZCore、JZNet |
| **JZFrp** | FRP 内网穿透模块 | JZNet |
| **JZBle** | 蓝牙通信模块 | JZCore |
| **JZHttpServer** | HTTP 服务器模块 | JZNet |
| **JZSensor** | 传感器模块（温度/湿度/气压等，驱动可扩展） | JZCore |

### 二、CMake 集成

#### 基础集成

首先使用include JZFramework.cmake,在指明需要使用的模块
```cmake
include(JZFramework/JZFramework.cmake)
```
使用jzframework_use_modules指定需要使用的模块，会自动包含依赖模块

引入核心模块：
```
jzframework_use_modules(JZCore)
target_link_libraries(your_target JZFramework)
```
引入网络通信模块：
```cmake
jzframework_use_modules(JZNet)
```
如果需要https
```cmake
target_compile_definitions(JZFramework PRIVATE MG_TLS=MG_TLS_BUILTIN)
```

引入协程模块：  
```cmake
jzframework_use_modules(JZCoroutine)
```
其他模块请参考JZFramework目录,每个子目录代表一个模块。


#### 自定义目标集成

```cmake
add_library(MyApp STATIC "")
jzframework_target_modules(MyApp JZCore JZNet)
```

#### 模块系统细节与可选依赖 (条件编译)

每个模块对应一个子目录 + 一个 `<模块名>.cmake` 配置文件，通过 `MODULE_DEPENDS_<模块名>`
声明依赖，`jzframework_use_modules` 会递归补齐依赖。两遍处理：

1. **声明遍**：给 JZFramework 目标挂 `HAS_<模块名>` 编译定义（所有请求的模块+依赖都会挂），
   C++ 代码里可用 `#ifdef HAS_JZMcpServer` 做源码级开关；
2. **添加遍**：按依赖顺序 include 各模块的 `<模块名>.cmake`，模块在其中把自己源文件
   `target_sources` 到 `${JZ_MODULE_TARGET}`（即 JZFramework 目标）。

**可选依赖**（B 模块存在时，A 模块才编译某些增强源文件）：CMake 侧直接 `if(HAS_<模块名>)`，
命中后追加源文件：

```cmake
# JZWidget.cmake 片段: JZMcpServer 参与构建时，把语义树/截图/驱动暴露为 MCP 工具
if(HAS_JZMcpServer)
    set(JZ_AI_SOURCES
        JZUiMcpTools.h
        JZUiMcpTools.cpp
    )
    convert_to_abs_paths(JZ_AI_SOURCES)
    target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZ_AI_SOURCES})
endif()
```

**`HAS_<模块名>` 的来历与语义**：`jzframework_use_modules` 在 include 任何模块 cmake
**之前**，先把"请求的模块 + 传递依赖"一次性收全，导出成 `HAS_<模块名>` 缓存变量
（见 JZFramework.cmake "模块启用状态"）。所以：

- 它是**缓存变量**，任何目录都能直接 `if(HAS_xxx)`，不需要先"查询"；
- 与模块在 `jzframework_use_modules` 列表里的**先后顺序无关**（不必把可选依赖排在前面）；
- 未启用的模块是"未定义"而非 OFF，`if(HAS_xxx)` 为假；
- 需要把结果存成变量时可用全局函数 `jz_module_exists(<模块> <结果变量>)`（任何作用域都能调）；
- C++ 侧对应的 `#ifdef HAS_<模块名>` 同名同语义（含传递依赖），两边可以对照看。

### 三、核心组件使用

#### 3.1 应用程序入口

```cpp
#include "JZCore/JZApplication.h"
#include "JZCore/JZLog.h"

int main(int argc, char *argv[])
{
    JZApplication app;
    LOG_I("Application started");
    return app.exec();
}
```

#### 3.2 对象系统

继承 JZObject：

```cpp
#include "JZCore/JZObject.h"

class MyClass : public JZObject
{
    JZ_OBJECT_DECLARE(MyClass)
    
public:
    MyClass(JZObject *parent = nullptr) : JZObject(parent) {}
};

JZ_OBJECT_IMPL(MyClass, JZObject)
```

#### 3.3 信号槽机制

定义信号：

```cpp
class MySender : public JZObject
{
    JZ_OBJECT_DECLARE(MySender)
    
public:
    MySender(JZObject *parent = nullptr) : JZObject(parent) {}
    
    void sendData(const std::string &data) {
        sigDataReady.emit(data);
    }
    
jz_signals:
    JZSignal<const std::string&> sigDataReady;
};

JZ_OBJECT_IMPL(MySender, JZObject)
```

信号需要在创建时传入当前object指针：
MySender::MySender(JZObject *parent) 
    : JZObject(parent) , sigDataReady(this)
{
}

连接信号到槽：

```cpp
// 连接到成员函数
connect(&sender.sigDataReady, &receiver, &MyReceiver::onDataReady);

// 连接到 lambda
connect(&sender.sigDataReady, [](const std::string &data) {
    LOG_I("Received: " + data);
});
```

连接类型：

| 类型 | 说明 |
|------|------|
| AutoConnection | 自动判断同步或异步（默认） |
| DirectConnection | 直接调用（同步） |
| QueuedConnection | 队列调用（异步） |
| BlockingQueuedConnection | 阻塞队列调用 |

#### 3.4 线程管理

创建线程：

```cpp
class WorkerThread : public JZThread
{
    JZ_OBJECT_DECLARE(WorkerThread)
    
protected:
    void run() override {
        LOG_I("Thread started");
        msleep(1000);
        LOG_I("Thread finished");
    }
};

JZ_OBJECT_IMPL(WorkerThread, JZThread)

// 使用
WorkerThread *thread = new WorkerThread();
thread->start();
```

在指定线程执行：

```cpp
runOnThread(jzApp->mainThread(), [](){
    // 在主线程执行
});
```

#### 3.5 日志系统

```cpp
LOG_D("Debug message");
LOG_I("Info message");
LOG_W("Warning message");
LOG_E("Error message");
LOG_F("Fatal message");

// 带格式化
LOG_FMT_I("User %s logged in, id=%d", "admin", 123);
```

#### 3.6 网络通信

HTTP 请求：

```cpp
#include "JZNet/JZNetworkAccessManager.h"

JZNetworkAccessManager manager;
JZNetworkRequest request("http://example.com/api/data");

// GET 请求
JZNetworkReply *reply = manager.get(request);
connect(reply, &JZNetworkReply::sigFinished, [reply]() {
    if (reply->error() == JZNetworkReply::NoError) {
        LOG_I("Response: " + reply->readAll().toString());
    }
    reply->deleteLater();
});
```

WebSocket：

```cpp
#include "JZNet/JZWebSocket.h"

JZWebSocket *ws = new JZWebSocket();
connect(ws, &JZWebSocket::sigConnected, [](){ LOG_I("Connected"); });
connect(ws, &JZWebSocket::sigTextMessageReceived, [](const std::string &msg){
    LOG_I("Received: " + msg);
});

ws->open("ws://localhost:8080/ws");
ws->sendText("Hello Server");
```

#### 3.7 协程编程

```cpp
#include "JZCoroutine/JZCo.h"

void coroutineTask() {
    LOG_I("Coroutine started");
    jzco_sleep(1000);  // 非阻塞睡眠
    LOG_I("Coroutine resumed");
}

int main() {
    jzco_init();
    
    jzco_spawn(coroutineTask);

    jzco_waitAllCoroutine();
    return 0;
}
```

#### 3.8 定时器

```cpp
m_timer = new JZTimer(this);
m_timer->setInterval(1000);  // 1秒
connect(m_timer, &JZTimer::sigTimeout, this, &MyClass::onTimeout);
m_timer->start();
```

### 四、关键类和头文件对应

| 功能 | 头文件 |
|------|--------|
| 应用程序 | `JZCore/JZApplication.h` |
| 对象基类 | `JZCore/JZObject.h` |
| 信号槽 | `JZCore/JZSignal.h` |
| 线程 | `JZCore/JZThread.h` |
| 日志 | `JZCore/JZLog.h` |
| 文件 | `JZCore/JZFile.h` |
| 网络 | `JZNet/JZNetworkAccessManager.h` |
| WebSocket | `JZNet/JZWebSocket.h` |
| 协程 | `JZCoroutine/JZCo.h` |
| 定时器 | `JZCore/JZTimer.h` |

### 五、最佳实践

1. **内存管理**：使用 `deleteLater` 延迟删除对象
2. **信号槽**：避免循环引用，使用 `QueuedConnection` 进行线程安全通信
3. **协程**：使用 `jzco_sleep` 而非 `msleep`，前者是非阻塞的
4. **日志**：根据模块分类使用不同的日志级别

### 六、快速参考

```cpp
// 初始化应用
JZApplication app;

// 发射信号
sigReady.emit("data");

// 启动线程
thread->start();

// 启动协程
jzco_spawn([](){ /* code */ });

// 定时器
m_timer->start();

// 发送事件
jzApp->postEvent(obj, new MyEvent());
```
