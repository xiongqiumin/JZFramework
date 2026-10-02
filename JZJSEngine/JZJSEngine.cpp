#include "JZJSEngine.h"

#include <assert.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <cstdlib>

#include "JZJSBind.h"
#include "JZJSWorkerThread.h"
#include "JZJSWorkerModule.h"
#include "JZApplication.h"
#include "JZDateTime.h"
#include "JZDir.h"
#include "JZElapsedTimer.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZLog.h"
#include "JZPlatform.h"
#include "JZStd.h"
#ifdef HAS_JZWidget
#include "JZWidget/JZWindowHost.h"   // 窗口宿主(见 setWindowHost): "之后 new 的窗口归谁托管"
#endif

using namespace jsbind;
using namespace std;


// 事件驱动唤醒事件: C++ 侧结算 promise / 外部注入任务后投递本事件到引擎线程,
// 引擎收到后 execOnce 泵一轮。JZObject 事件分发: type >= JZEvent_user 的自定义
// 事件统一进 customEvent(见 JZObject::event)。
static const int JZEngine_WakeUp = JZEvent_user + 1;

// ---------------------------------------------------------------------------
// Node 风格"内置模块"表
//
// 对齐 Node 的关键点: Node 里 `import http from 'http'` 之所以能用裸名, 是因为 loader
// 认一张内置名表, 不需要 node_modules。本引擎没有 node_modules, 所以这张表就是它的等价物 ——
// 而且 allowlist 式的查找比"按路径找文件"更快。
//
// ⭐ 布局原则(2026-09-22 起): **一个名字 = 一个包目录, 目录名就是名字**
//   js_wrapper/<名字>/{index.ts, types/index.d.ts, types/ambient.d.ts, dist/}
//   ⇒ 表里 file 就是"它自己的 dist 入口", 不再有"名字 A 藏在包 B 里"(以前 http 在 net 包里、
//     bytes 在 core 包里、os 还在两个包里合并)。类型文件与实现在同一个目录, 查定义只需看名字。
//
// ⚠ 只支持 ESM: 没有 require, 也不认 module.exports / exports。写 CommonJS 会报
//    "require is not defined"。
//
// file   : 包 dist 出口, 写"相对 libraryDirs 的裸名"(见 readModuleCode 的候选解析)。
// exportName: 该出口里用来当 default 的具名导出:
//            nullptr = 用包自带的 default(现在的常规形态:
//                      每个包的 index.ts 都 `export default <模块对象>` + 具名导出各成员,
//                      于是 `import http from 'http'` 与 `import { createServer } from 'http'` 都成立);
//            "*"     = namespace 形态(只有具名导出, 没有 default)。
//
// ⚠ 加一行名字要同步三处(与 Node 那边 '@types/node 按同名给类型' 是一回事):
//   1) 本表;
//   2) 该包 types/ambient.d.ts 里的 `declare module '<名字>' { export * from './index' ... }`;
//   3) 类型清单 js_wrapper/types/index.d.ts 的内置名列表(与提示词里的内置名列表)。
//   缺 2) 的症状: 编译期 TS2307(名字没有类型声明), 运行期却是好的 —— 模型会被绊一次。
// ---------------------------------------------------------------------------
struct BuiltinModule
{
    const char* name;        // 裸模块名(不含 node: 前缀); 同时也是包目录名
    const char* file;        // 该名字的 dist 入口(相对 libraryDirs 的裸名路径)
    const char* exportName;  // 用来当 default 的具名导出; nullptr = 用包自带 default; "*" = namespace
};

static const BuiltinModule builtinModules[] = {
    // ---- 网络类(每个名字一个包) ----
    { "net",        "net/dist/index.js",        nullptr },   // net 对象的 default + Socket/Server/... 具名
    { "dgram",      "dgram/dist/index.js",      nullptr },
    { "http",       "http/dist/index.js",       nullptr },   // http.createServer 与具名 createServer 都在
    { "mqtt",       "mqtt/dist/index.js",       nullptr },
    { "ws",         "ws/dist/index.js",         nullptr },   // 客户端类还同时是全局 WebSocket
    { "url",        "url/dist/index.js",        nullptr },   // URL / URLSearchParams 同时是全局
    { "os",         "os/dist/index.js",         nullptr },   // 以前 cross-package 合并, 现在一个包
    { "events",     "events/dist/index.js",     nullptr },   // EventEmitter 的**唯一实现**(同时是全局)
    // ---- 系统类 ----
    { "fs",         "fs/dist/index.js",         nullptr },
    { "path",       "path/dist/index.js",       nullptr },
    { "process",    "process/dist/index.js",    nullptr },
    { "crypto",     "crypto/dist/index.js",     nullptr },
    { "buffer",     "buffer/dist/index.js",     nullptr },   // Node Buffer 兼容(编码转换 + 数值读写) + TextEncoder/TextDecoder 全局(零依赖)
    { "stream",     "stream/dist/index.js",     nullptr },   // Node stream 兼容子集(零依赖)
    { "worker_threads", "worker_threads/dist/index.js", nullptr },   // Node worker_threads 兼容层(包 __jz.worker)
    // ---- 设备 / 数据库 ----
    { "serialport", "serialport/dist/index.js", nullptr },
    { "modbus",     "modbus/dist/index.js",     nullptr },
    { "sqlite",     "sqlite/dist/index.js",     nullptr },   // 全同步 API, 对齐 node:sqlite(另有 default)
    // ---- 控件族(特殊: 包入口由引擎按路径先加载, 这里只是让裸名 import 也成立) ----
    // "*" = namespace 形态(包入口只有具名导出): `import { JZWindow } from 'widget'` 运行期可用。
    // 类层是**幂等**的(见 widget/classes.ts 的说明), 所以"引擎按路径加载过一次 + 用户再按名字
    // import"不会重建类、不会换掉原型。
    { "widget",     "widget/dist/index.js",     "*"     },
    // ---- computer use(控件树 + UI 操作在 JS; 底层动作走 __jz.computeruse) ----
    // "*" = namespace 形态(包入口只有具名导出, 没有 default):
    //   import { App, setBackend } from 'computeruse';
    // ⚠ 本包只"能用"当且仅当宿主注册了 __jz.computeruse(C++ JZJSComputerUseModule);
    //   否则默认后端在第一次调用时抛异常(模块本身能 import 成功)。
    //   离线/模拟环境用 setBackend() 注入自己的后端即可, 不依赖 C++ 侧。
    { "computeruse", "computeruse/dist/index.js", "*"    },
    // ---- computer vision(OpenCV 的传统图像处理: 滤波/颜色转换/形态学/阈值/轮廓/绘制...) ----
    // 常规形态(nullptr = 具名 + 包自带 default 都暴露): 入口是 OpenCV.js 形状的命名空间,
    // 于是两种写法都成立 ——
    //   import cv from 'cv';               // OpenCV.js 习惯: cv.cvtColor / cv.COLOR_BGR2GRAY
    //   import { Mat, cvtColor } from 'cv'; // 只要单个成员
    // ⚠ 能不能真用取决于两件事: 宿主装没装 __jz.cv(C++ JZJSCvModule), 以及那个 exe 编没编
    //   OpenCV(JZ_CU_OPENCV)。没编 OpenCV 时模块照样 import 得进来, 但 available() 为 false、
    //   调用抛 notBuilt —— 刻意让"形状一致"(而不是 import 直接失败), 脚本好判定、好降级。
    { "cv",         "cv/dist/index.js",         nullptr },
    // ---- 音频(录音/放音/设备/WAV/重采样): __jz.audio, 由宿主的 JZJSAudioModule 挂载 ----
    // 能不能真用取决于宿主装没装 __jz.audio(JZJSAudioModule) 与该 exe 编没编 JZAudio(HAS_JZAudio)。
    // 没编时模块照样 import 得进来, 但 available() 为 false、调用抛 notBuilt(形状一致, 脚本好降级)。
    { "audio",      "audio/dist/index.js",      nullptr },
    // ---- ONNX Runtime 推理(跑任意 .onnx 模型: 加载/看输入输出/喂张量/拿张量) ----
    // "*" = namespace 形态。能不能用取决于宿主装没装 __jz.onnx 与该 exe 编没编 ORT(JZ_ONNX);
    //   没编时模块照样 import 得进来, 但 available() 为 false、调用抛 notBuilt(形状一致)。
    { "onnx",       "onnx/dist/index.js",       "*"     },
    // ---- OCR(本地 PP-OCR: 读屏幕上的字, 不依赖外部服务) ----
    // 底层在 __jz.computeruse 那组(initOcr/ocrTexts/match{kind:'ocr'})上 —— 本包只是它的
    // TS 门面(built/ready 分开问、支持包含/全等/正则、返回文本与分数)。
    // 能不能用取决于宿主装没装 __jz.computeruse 与该 exe 编没编 OCR(JZ_CU_OCR)。
    { "ocr",        "ocr/dist/index.js",        "*"     },
};

// 剥掉 Node 的 node: 前缀('node:http' -> 'http'), 并去掉空白字符。
static string normalizeBuiltinName(const string& specifier)
{
    string name;
    for (char c : specifier)
    {
        if (c > ' ')
            name.push_back(c);
    }
    const string prefix = "node:";
    if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
        name.erase(0, prefix.size());
    return name;
}

static const BuiltinModule* findBuiltinModule(const string& specifier)
{
    string name = normalizeBuiltinName(specifier);
    if (name.empty())
        return nullptr;

    for (size_t i = 0; i < sizeof(builtinModules) / sizeof(builtinModules[0]); i++)
    {
        if (name == builtinModules[i].name)
            return &builtinModules[i];
    }
    return nullptr;
}

class JZJSEngineInternalFunction
{
public:
    static JSValue js_sleep(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
    {
        if (argc != 1)
        {
            return JS_ThrowTypeError(ctx, "参数错误: 需要函数和时间间隔");
        }

        int interval_ms;
        if (JS_ToInt32(ctx, &interval_ms, argv[0]) != 0 || interval_ms <= 0)
        {
            return JS_ThrowTypeError(ctx, "时间间隔必须是正整数");
        }

        JZThread::msleep(interval_ms);
        return JS_UNDEFINED;
    }

    static JSValue js_set_interval(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        if (argc < 2 || !JS_IsFunction(ctx, argv[0]))
        {
            return JS_ThrowTypeError(ctx, "参数错误: 需要函数和时间间隔");
        }

        int interval_ms;
        if (JS_ToInt32(ctx, &interval_ms, argv[1]) != 0)
        {
            return JS_ThrowTypeError(ctx, "参数错误: 时间间隔必须是整数");
        }
        if (interval_ms < 0)
        {
            return JS_ThrowTypeError(ctx, "时间间隔不能为负数");
        }
        // setInterval 周期任务若为 0, 重排时 next_run_time += 0 永不前进,
        // 会让 execOnce 的 while(true) 死循环, 故仍要求正整数;
        // setTimeout(magic==1) 允许 0 = 尽快在下一轮 execOnce 触发
        if (magic == 0 && interval_ms == 0)
        {
            return JS_ThrowTypeError(ctx, "时间间隔必须是正整数");
        }

        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        int id;
        if (magic == 0)
            id = engine->setInterval(argv[0], interval_ms, false);
        else
            id = engine->setInterval(argv[0], interval_ms, true);

        return makeTimeoutObject(ctx, id);
    }

    static JSValue js_clear_interval(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        if (argc < 1)
        {
            return JS_ThrowTypeError(ctx, "参数错误: 时间间隔");
        }

        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);

        // Node 的 clearTimeout/clearInterval 两种入参都收: Timeout 对象, 或数字 id。
        if (JS_IsObject(argv[0]))
        {
            JSValue id_val = JS_GetPropertyStr(ctx, argv[0], "_id");
            if (JS_IsException(id_val))
            {
                JS_FreeValue(ctx, JS_GetException(ctx));   // 取属性本身抛错: 当作"不是 Timeout"
                return JS_UNDEFINED;
            }
            bool ok = JS_IsNumber(id_val);
            int id = 0;
            if (ok && JS_ToInt32(ctx, &id, id_val) != 0)
                ok = false;
            JS_FreeValue(ctx, id_val);

            if (ok)
                engine->clearInterval(id);
            // 不是本引擎的 Timeout(没有 _id): 与 Node 一样静默忽略, 不抛
            return JS_UNDEFINED;
        }

        int id;
        if (JS_ToInt32(ctx, &id, argv[0]) != 0)
        {
            return JS_ThrowTypeError(ctx, "时间间隔必须是正整数");
        }
        engine->clearInterval(id);

        return JS_UNDEFINED;
    }

    // ---- Node 风格 Timeout 对象(定时器返回值) ----
    // Node 的 setTimeout/setInterval 返回的是 Timeout 实例(带 ref/unref/hasRef/refresh), 不是数字。
    // 这里照做; 另补 valueOf() 让"把返回值当数字用"的老代码(+t / 比较 / clearInterval(+t))仍然成立。
    // 注意: ref/unref 只改"是否算一票让宿主活着"这一属性, 不影响定时器本身照常触发。
    static JSValue js_timer_ref(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        ((JZJSEngine*)JS_GetContextOpaque(ctx))->setTimerRef((uint32_t)magic, true);
        return JS_DupValue(ctx, this_val);   // Node: 返回 Timeout 本身, 可链式
    }

    static JSValue js_timer_unref(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        ((JZJSEngine*)JS_GetContextOpaque(ctx))->setTimerRef((uint32_t)magic, false);
        return JS_DupValue(ctx, this_val);
    }

    static JSValue js_timer_has_ref(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        bool refed = ((JZJSEngine*)JS_GetContextOpaque(ctx))->timerHasRef((uint32_t)magic);
        return JS_NewBool(ctx, refed);
    }

    static JSValue js_timer_refresh(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        int64_t interval = 0;   // 0 = 沿用原间隔
        if (argc >= 1 && !JS_IsUndefined(argv[0]))
        {
            int interval_ms = 0;
            if (JS_ToInt32(ctx, &interval_ms, argv[0]) != 0 || interval_ms < 0)
                return JS_ThrowTypeError(ctx, "时间间隔必须是正整数");
            interval = interval_ms;
        }
        ((JZJSEngine*)JS_GetContextOpaque(ctx))->refreshTimer((uint32_t)magic, interval);
        return JS_DupValue(ctx, this_val);
    }

    static JSValue js_timer_valueof(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        return JS_NewInt32(ctx, magic);   // 当数字用时 = 任务 id(老代码兼容)
    }

    static JSValue makeTimeoutObject(JSContext* ctx, int id)
    {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "_id", JS_NewInt32(ctx, id));
        JS_SetPropertyStr(ctx, obj, "ref",
                          JS_NewCFunctionMagic(ctx, js_timer_ref, "ref", 0, JS_CFUNC_generic_magic, id));
        JS_SetPropertyStr(ctx, obj, "unref",
                          JS_NewCFunctionMagic(ctx, js_timer_unref, "unref", 0, JS_CFUNC_generic_magic, id));
        JS_SetPropertyStr(ctx, obj, "hasRef",
                          JS_NewCFunctionMagic(ctx, js_timer_has_ref, "hasRef", 0, JS_CFUNC_generic_magic, id));
        JS_SetPropertyStr(ctx, obj, "refresh",
                          JS_NewCFunctionMagic(ctx, js_timer_refresh, "refresh", 1, JS_CFUNC_generic_magic, id));
        JS_SetPropertyStr(ctx, obj, "valueOf",
                          JS_NewCFunctionMagic(ctx, js_timer_valueof, "valueOf", 0, JS_CFUNC_generic_magic, id));
        return obj;
    }

    static JSValue js_is_interruption(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        bool flag = engine->isInterruption();
        return JS_NewBool(ctx, flag);
    }

    // console.log / error / warn: node 的 util.format 规则(见 JZJSEngine::print/nodeFormat)
    static JSValue js_console_log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        engine->print(this_val, argc, argv);
        return JS_UNDEFINED;
    }

    // console.debugPrint: JZ 原来的紧凑格式(见 JZJSEngine::debugPrint)
    static JSValue js_console_debug_print(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        engine->debugPrint(this_val, argc, argv);
        return JS_UNDEFINED;
    }

    static JSValue js_fs_readString(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);

        if (argc != 1)
            return engine->makeParamNumError(1);

        string path;
        if (!js_to_value(ctx, argv[0], path))
            return engine->makeTypeError(0, JSType_string);

        path = engine->fullPath(path);

        JZFile file;
        if (!file.open(path.c_str(), "rb"))
            return engine->makeOpenFileError(path);

        JZByteArray result = file.readAll();
        file.close();
        return JS_NewString(ctx, result.data());
    }

    // registFunction / registAsyncFunction 注册的是"挂在某个 JS 对象上的普通函数"(全局 / 命名空间 /
    // 模块导出, 如 process.cwd / fs.xxx): this 只是"函数挂在谁身上", 不是类实例 -> **不占"当前 this"帧**
    // (见 JZJSEngine::SelfScope 的判据)。这类函数里 self() 看到的是外层的实例。
    static JSValue js_cfunction(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        return engine->call(magic, argc, argv);
    }

    static JSValue js_async_cfunction(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        return engine->callAsync(magic, argc, argv);
    }

    static JSValue js_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv, int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        // create 里的参数校验 / 类型转换(fromJSValue)会抛 C++ 异常, 与 call() 一致地
        // 转成 JS 异常 —— 否则异常会跨 quickjs 的 C 调用栈展开。
        try
        {
            return engine->createValue(magic, argc, argv);
        }
        catch (std::exception& e)
        {
            return JS_ThrowInternalError(ctx, "%s", e.what());
        }
    }

    static void js_deconstructor(JSRuntime* rt, JSValue val)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetRuntimeOpaque(rt);
        engine->deconstructor(val);
    }

    static JSValue js_member_function(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        // magic 版: 只有类实例方法才占"当前 this"这一帧(见 JZJSEngine::SelfScope 的判据)
        JZJSEngine::SelfScope self_scope(engine, magic, this_val);
        std::vector<JSValue> args;
        args.push_back(this_val);
        for (int i = 0; i < argc; i++)
            args.push_back(argv[i]);
        return engine->call(magic, (int)args.size(), args.data());
    }

    static JSValue js_property_get(JSContext* ctx, JSValue this_val, int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        JZJSEngine::SelfScope self_scope(engine, magic, this_val);

        std::vector<JSValue> args;
        args.push_back(this_val);
        return engine->call(magic, (int)args.size(), args.data());
    }

    static JSValue js_property_set(JSContext* ctx, JSValue this_val, JSValue val, int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        // 属性 setter 的 magic 是 getter 的 magic + 1(见 registClass), 照原样判即可
        JZJSEngine::SelfScope self_scope(engine, magic, this_val);

        std::vector<JSValue> args;
        args.push_back(this_val);
        args.push_back(val);
        return engine->call(magic + 1, (int)args.size(), args.data());
    }

    // 全局 self(): 返回"当前正在被 JS 调用的那个对象"(即该 C++ 方法被调时的 this)。
    // quickjs 没有这个 API(见 JZJSEngine::self 的说明), 引擎自己维护调用栈, 这里取栈顶。
    // 不在 JS -> C++ 的成员调用里(比如模块顶层)返回**真 null**, 便于 JS 侧判空。
    static JSValue js_self(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        JSValue v = engine ? engine->self() : JS_UNDEFINED;
        if (JS_IsUndefined(v))
            return JS_NULL;
        return JS_DupValue(ctx, v);   // 交给 JS 的那份要 dup(栈里那份是借用)
    }

    static void js_promise_reject(JSContext* ctx, JSValueConst promise, JSValueConst reason, bool is_handled,
                                  void* opaque)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        if (!engine)
            return;

        // is_handled=true = "有人刚给这个已拒绝的 promise 挂了 handler" ⇒ **撤销**先前记下的
        // 未处理拒绝, 而不是简单忽略(忽略的话那条错误还留在待报列表里, 宿主仍会收到运行时错误)。
        if (is_handled)
        {
            engine->promiseRejectHandled(promise);
            return;
        }

        engine->unHandlePromiseReject(promise, reason);
    }

    // C++ 调用 JS 异步函数(callAsyncFunction)挂到 promise.then 的结算回调:
    // promise 在引擎 job 泵中结算时被 quickjs 调用, magic = callAsyncFunction 句柄
    static JSValue js_call_async_resolve(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv,
                                         int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        engine->asyncResultResolve(magic, argc, argv);
        return JS_UNDEFINED;
    }

    static JSValue js_call_async_reject(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv,
                                        int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        engine->asyncResultReject(magic, argc, argv);
        return JS_UNDEFINED;
    }

    // ESM 模块 loader: 编译指定的模块文件为模块定义。
    // module_name 已被 quickjs 默认 normalizer 归一化(如 "build/sort.js")。
    static JSModuleDef* js_module_loader(JSContext* ctx, const char* module_name, void* opaque)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        return engine->loadModuleDef(module_name);
    }
};
using JSInter = JZJSEngineInternalFunction;

// JZJSValuePtr
JZJSValuePtr::JZJSValuePtr()
{
    m_value = JS_UNDEFINED;
}

JZJSValuePtr::JZJSValuePtr(JSContext* ctx, JSValue value)
{
    m_value = value;
    m_guard = std::shared_ptr<void>((void*)nullptr, [ctx, value](void*) { JS_FreeValue(ctx, value); });
}

JZJSValuePtr::~JZJSValuePtr()
{
}

JSValue JZJSValuePtr::get() const
{
    return m_value;
}

// JZJSEngine
JZJSEngine::TypeInfo::TypeInfo()
{
    type = JSType_none;
    valueType = JSType_none;
    jsType = JS_TAG_UNDEFINED;
}

JZJSEngine::TimerTask::TimerTask()
{
    runOnce = false;
    refed = true;       // 对齐 Node: 定时器默认 ref(吊住宿主), 要放行得自己 unref()
    callback = JS_UNDEFINED;
    interval = 0;
    next_run_time = 0;
    id = 0;
}

JZJSEngine::PromiseTask::PromiseTask()
{
    ctx = nullptr;
    resolve = JS_UNDEFINED;
    reject = JS_UNDEFINED;
}

JZJSEngine::PromiseTask::~PromiseTask()
{
    if (ctx)
    {
        // 必须释放 resolve 和 reject 两个闭包, 否则在 JS_FreeRuntime 时
        // gc_obj_list 仍非空, 触发 list_empty 断言
        JS_FreeValue(ctx, resolve);
        JS_FreeValue(ctx, reject);
    }
}

JZJSEngine::JZJSEngine(JZObject* parent)
    : JZObject(parent), sigStatus(this), sigLog(this), sigError(this), sigAsyncResult(this)
{
    m_ctx = nullptr;
    m_rt = nullptr;
    m_global = JS_UNDEFINED;

    m_timeTaskId = 0;
    m_promiseTaskId = 0;
    m_objectId = JSType_objectUser;

    m_timer = INVALID_TIMER_ID;
    m_timerExpire = 0;
    m_pendingWork = 0;
    m_interrupt = false;
    m_processExitCode = 0;
    m_processExitCodeSet = false;
    m_processExitRequested = false;
    m_status = Engine_none;    
    m_nextWorkerId = 1;         // 0 保留给主线程
    m_workerModule = nullptr;
    m_widgetModule = nullptr;
}

JZJSEngine::~JZJSEngine()
{
    // 兜底: 宿主不一定显式调 deinit()(测试/临时引擎很常见)。worker 线程表就在引擎里,
    // 必须在这里收掉, 否则:
    //   1) 线程对象泄漏;
    //   2) m_mainEngine 悬挂 —— 还在跑的 worker 线程去 mainEngine() 会解引用已销毁的
    //      引擎, 直接崩(0xC0000005)。
    // 注意: 这里只清"进程级资源", 不碰 JS 侧的 rt/ctx —— 那仍由 deinit() 负责,
    // 以免破坏"借给 JS 的 C++ 对象"的既有生命周期约定。
#ifdef HAS_JZWidget
    // 窗口宿主也是"进程级资源": 引擎没 deinit 就析构时(临时引擎很常见)也得摘掉,
    // 否则之后 new 出来的窗口会去找一具已经析构的宿主。
    if (m_windowHost && jzWindowHost() == m_windowHost)
        jzSetWindowHost(nullptr);
#endif
    reapAllWorkers();
}

JZJSEngine* JZJSEngine::mainEngine()
{
    return m_mainEngine;
}

void JZJSEngine::init()
{
    assert(!m_ctx);
    initJSEnv();

    // 没显式设过运行根(宿主/测试都常见)就默认用**进程当前目录**: 这样 __jz.core.workDir()、
    // 裸模块名的解析基目录、JS 的 process.cwd() 都不会是空串。宿主之后 setWorkDir() 覆盖即可。
    if (m_workDir.empty())
        m_workDir = JZDir::currentPath();

    // 第一个 init 的引擎成为主引擎: worker 线程表与 id 分配都归它。
    // 主线程自己也是一个可投递的 id(0), 所以表里先放它自己。
    if (!m_mainEngine)
    {
        m_mainEngine = this;

        WorkerEntry& e = m_workers[0];
        e.parentId = -1;
        e.ownerId = 0;
        e.exited = false;
        e.thread = nullptr;
        e.engine = this;
    }

#ifdef HAS_JZWidget
    // 窗口宿主: 从这一刻起, 本引擎里 new 出来的窗口(JS 的 new JZWindow 也在内)都归它托管
    // (见 JZWindowHost —— 例如虚拟桌面: 建出来的窗口全落在虚拟桌面上)。没装宿主就是真窗口。
    if (m_windowHost)
        jzSetWindowHost(m_windowHost);
#endif
}

void JZJSEngine::deinit()
{
        // 主引擎: 先收掉所有 worker 线程(请求退出 + join + delete, 必须在主线程做)
    reapAllWorkers();
    if (m_mainEngine == this)
        m_mainEngine = nullptr;

#ifdef HAS_JZWidget
    // 引擎要没了: 摘掉窗口宿主 —— 别让之后新建的窗口去找一具已经析构的宿主。
    // (只有"当前装的就是我这份"时才摘, 免得误摘别人装的。)
    if (m_windowHost && jzWindowHost() == m_windowHost)
        jzSetWindowHost(nullptr);
#endif

    // 注: 旧"指针身份表 + 托管句柄"已随包装层一起删除(2026-09-17), 这里不再有需要统一放掉的句柄 ——
    //     对象的 JS 侧状态(opaque 里的守卫 / C++ 对象上的 js_value)都随各自的对象生命周期走。

    deinitJSEnv();
}

void JZJSEngine::setWorkDir(const std::string& dir)
{
    m_workDir = dir;
}

// "当前 this": 见头文件里的说明。栈里存的是借用值(被调用方 this_val 的形参), 不 dup/不释放。
JSValue JZJSEngine::self() const
{
    if (m_selfStack.empty())
        return JS_UNDEFINED;
    return m_selfStack.back();
}

void JZJSEngine::setLibraryDir(const std::vector<std::string>& dirs)
{
    m_libraryDirs = dirs;
}

void JZJSEngine::initJSEnv()
{
    initTypeMap();

    m_objectId = JSType_objectUser;
    m_timeTaskId = 0;
    m_promiseTaskId = 0;
    m_interrupt = false;

    // process.exit/exitCode 的状态(每次重建会话都要清, 见 requestProcessExit)
    m_processExitCode = 0;
    m_processExitCodeSet = false;
    m_processExitRequested = false;

    m_rt = JS_NewRuntime();
    m_ctx = JS_NewContext(m_rt);
    m_global = JS_GetGlobalObject(m_ctx);

    JS_SetHostPromiseRejectionTracker(m_rt, &JSInter::js_promise_reject, nullptr);
    JS_SetModuleLoaderFunc(m_rt, NULL, &JSInter::js_module_loader, this);

    registInternal();
    registVector<int>();
    registVector<double>();
    registVector<string>();

    registConsole();

    // JS 回调的 GC 根数组: 挂一份到全局(__jzCallbackRoot), 自己持一份 —— 于是"只被 C++ 侧
    // 持有的回调闭包"也始终从根可达, 不会被 quickjs 的循环 GC 回收(见 retainCallback 说明)。
    m_cbRoot = JS_NewArray(m_ctx);
    m_cbId = 0;
    JS_SetPropertyStr(m_ctx, m_global, "__jzCallbackRoot", JS_DupValue(m_ctx, m_cbRoot));

    setStatus(Engine_pause);
}

void JZJSEngine::deinitJSEnv()
{
    stopEngineTimer();
    m_selfStack.clear();   // 借用值, 直接丢(不该还有值: 这里已不在 JS 调用栈里)
    if (m_rt)
    {
        clearPromiseRejects();   // 释放待报拒绝持有的 promise 引用(必须在 FreeContext 之前)
        JS_FreeValue(m_ctx, m_cbRoot);

        for (int i = 0; i < (int)m_timeTasks.size(); i++)
        {
            JS_FreeValue(m_ctx, m_timeTasks[i].callback);
        }
        m_timeTasks.clear();
        m_promiseTask.clear();

        JS_FreeValue(m_ctx, m_global);
        JS_FreeContext(m_ctx);
        JS_FreeRuntime(m_rt);
        setStatus(Engine_none);
    }
    m_cbRoot = JS_UNDEFINED;
    m_cbId = 0;

    m_global = JS_NULL;
    m_ctx = nullptr;
    m_rt = nullptr;

    m_workDir.clear();
    m_appFile.clear();
    m_promiseRejects.clear();   // 引用已在 FreeContext 之前释放(见上面 clearPromiseRejects)
    m_runtimeError.clear();
    m_funcList.clear();
    m_typeMap.clear();
    m_cTypeMap.clear();
    m_jsTypeMap.clear();
}

void JZJSEngine::setInterruption(bool flag)
{
    m_interrupt = flag;
}

bool JZJSEngine::isInterruption() const
{
    return m_interrupt;
}

// ---- process.exit / process.exitCode(对齐 Node; 规则表见 js_wrapper/process/index.ts) ----
void JZJSEngine::requestProcessExit(int code)
{
    m_processExitCode = code;
    m_processExitCodeSet = true;
    m_processExitRequested = true;

    // worker 引擎里的 process.exit() **只结束这个 worker**(对齐 Node: 不牵连宿主进程)。
    // 主引擎才真退宿主 —— code_runtime 的进程退出码 = app.exec() 的返回值(见 runtime/main.cpp)。
    JZJSEngine* main = mainEngine();
    JZJSWorkerModule* wm = workerModule();
    if (main && main != this && wm && wm->selfId() != 0)
    {
        main->terminateWorker(wm->selfId());
        return;
    }

    if (jzApp)
        jzApp->exit(code);
}

void JZJSEngine::setProcessExitCode(int code)
{
    m_processExitCode = code;
    m_processExitCodeSet = true;
}

void JZJSEngine::clearProcessExitCode()
{
    m_processExitCode = 0;
    m_processExitCodeSet = false;
}

int JZJSEngine::processExitCode() const
{
    return m_processExitCode;
}

bool JZJSEngine::processExitCodeSet() const
{
    return m_processExitCodeSet;
}

void JZJSEngine::wait(int delay)
{
    JZElapsedTimer t;
    t.start();
    while (t.elapsed() < delay)
    {
        if (isInterruption())
            return;

        JZThread::msleep(20);
    }
}

void JZJSEngine::initTypeMap()
{
    registBase("JSValue", JSType_oriValue, -100, typeid(JSValue).name());
    
    registBase("null", JSType_null, JS_TAG_NULL, typeid(nullptr_t).name());
    registBase("undefined", JSType_undefined, JS_TAG_UNDEFINED, "undefined");
    registBase("bool", JSType_bool, JS_TAG_BOOL, typeid(bool).name());
    registBase("int", JSType_int, JS_TAG_INT, typeid(int).name());
    registBase("double", JSType_double, JS_TAG_FLOAT64, typeid(double).name());
    registBase("string", JSType_string, JS_TAG_STRING, typeid(string).name());
    registBase("any", JSType_jsonValue, JS_TAG_OBJECT, typeid(Json::Value).name());

    // 透明控件类的"对象指针"形参类型(JSType_objectPointer): 控件族共用一个 op class, 在类型表里
    // 没有各自的类型号, 所以控件方法的对象形参统一声明成 `JZObjectPointer*` —— 引擎据此能在
    // **形参层**校验"必须是活着的对象指针"(见 checkFunctionArg 的 arg_type > JSType_object 分支)。
    // js_type 传一个不存在的 tag(-101, 与上面 JSValue 那条同款写法)以免污染 m_jsTypeMap。
    registBase("JZObjectPointer", JSType_objectPointer, -101, typeid(JZObjectPointer).name());
}

void JZJSEngine::registInternal()
{
    string init_code = R"(
// JZ 原来的"紧凑格式"打印器 —— 现在只给 **console.debugPrint** 用(字符串一律带双引号,
// `{a: 1}` 没有内边距, 对象走 for...in 所以能看到继承来的可枚举属性); console.log 用的是
// 下面那套 node 格式, 见 __nodeFormat__。错误描述(JZJSEngine::printObject)也用它。
function __printObject__(obj, seen = new WeakSet()) {
    // 处理循环引用
    if (seen.has(obj)) {
        return "[Circular]";
    }

    let result;
    // 处理 null（typeof null 是 'object'，需要单独判断）
    if (obj === null) {
        result = "null";
    }
    // 处理 Error 对象
    else if (obj instanceof Error) {
        result = `${obj.name || 'UnknownError'}: ${obj.message}`;
        // 可选：保留错误堆栈（如果需要）
        // if (obj.stack) result += `\n${obj.stack}`;
    }
    // 处理数组
    else if (Array.isArray(obj)) {
        seen.add(obj); // 标记数组已遍历，防止循环引用
        result = "[";
        // 使用 let 替代 var，避免变量提升问题
        for (let i = 0; i < obj.length; i++) {
            result += __printObject__(obj[i], seen);
            if (i !== obj.length - 1) {
                result += ", ";
            }
        }
        result += "]";
        seen.delete(obj); // 遍历完成后移除标记
    }
    // 处理普通对象
    else if (typeof obj === 'object') {
        seen.add(obj); // 标记对象已遍历，防止循环引用
        const strList = []; // 使用 const 替代 var，符合现代 JS 规范
        for (const key in obj) {
            strList.push(`${key}: ${__printObject__(obj[key], seen)}`);
        }
        result = `{${strList.join(", ")}}`;
        
        // 处理自定义原型对象的类名
        const proto = Object.getPrototypeOf(obj);
        if (proto !== null && proto !== Object.prototype) {
            const className = proto.constructor?.name;
            if (className) {
                result = `${className}${result}`;
            }
        }
        seen.delete(obj); // 遍历完成后移除标记
    }
    // 处理字符串（添加引号包裹）
    else if (typeof obj === 'string') {
        result = JSON.stringify(obj);
    }
    // 处理其他类型（数字、布尔、undefined 等）
    else {
        result = String(obj);
    }

    return result;
}

// ==================== console.log 的输出格式 (对齐 node 的 util.format + util.inspect) ====================
// console.log / error / warn / assert 走这一套(与 node 一致), 原来的紧凑格式留在 console.debugPrint:
//     console.log('hi')             -> hi              (顶层字符串不加引号)
//     console.log('a', 1, [2])      -> a 1 [ 2 ]       (多参数空格分隔; 容器带内边距)
//     console.log({ a: 'x' })       -> { a: 'x' }      (容器里的字符串用单引号)
//     console.log('%s=%d', 'n', 3)  -> n=3             (util.format 占位符)
//     console.log(new Date(0))      -> 1970-01-01T00:00:00.000Z
// 已知的简化(都是"少给一点信息", 不是格式跑偏 —— 需要时再往里补):
//   · 不按 72 列折行(一律单行)、不截断超长字符串、嵌套深度固定 2 层(更深显示 [Object]/[Array]);
//   · 循环引用只打 [Circular] (node 20 打 [Circular *1] 并在首现处标 <ref *1>);
//   · Promise 的状态从 JS 侧读不到 ⇒ 打 <unknown> (要与 node 完全一致得在 C 侧调 JS_PromiseState);
//   · 只列**自有可枚举**属性(与 node 一致; 于是 JZ 绑定的原型访问器不会列出来 —— 要看那些用
//     console.debugPrint);  · %o 与 %O 不区分(都不列不可枚举属性)。
function __nodeInspectQuoted__(str) {
    let out = "'";
    for (let i = 0; i < str.length; i++) {
        const ch = str[i];
        if (ch === "'") out += "\\'";
        else if (ch === "\\") out += "\\\\";
        else if (ch === "\n") out += "\\n";
        else if (ch === "\r") out += "\\r";
        else if (ch === "\t") out += "\\t";
        else out += ch;
    }
    return out + "'";
}

// 对象的键: 是合法标识符就裸写, 否则用引号包起来 (node 同款)
function __nodeInspectKey__(key) {
    return /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(key) ? key : __nodeInspectQuoted__(key);
}

// 空容器 node 不给内边距: `{}` / `[]` / `Map(0) {}`; 非空才前后各一个空格
function __nodeWrapBody__(head, body) {
    return body.length > 0 ? head + ' { ' + body + ' }' : head + ' {}';
}

// 取成员文本: 自有访问器属性不调用 getter, 与 node 一样只标 [Getter]/[Setter]
function __nodeInspectMember__(obj, key, depth, seen) {
    let d = null;
    try { d = Object.getOwnPropertyDescriptor(obj, key); } catch (e) { d = null; }
    if (d && d.get && d.set) return '[Getter/Setter]';
    if (d && d.get) return '[Getter]';
    if (d && d.set) return '[Setter]';
    return __nodeInspect__(obj[key], depth, seen);
}

function __nodeInspect__(value, depth, seen) {
    if (depth === undefined) depth = 2;          // node 的默认深度就是 2
    if (seen === undefined) seen = [];

    const type = typeof value;
    if (value === null) return 'null';
    if (value === undefined) return 'undefined';
    if (type === 'string') return __nodeInspectQuoted__(value);
    if (type === 'bigint') return String(value) + 'n';
    if (type === 'symbol') return String(value);
    // ⚠ -0: String(-0) 是 "0", 而 node 打 "-0" (Object.is 才分得出 -0 与 0)
    if (type === 'number') return Object.is(value, -0) ? '-0' : String(value);
    if (type === 'boolean') return String(value);
    if (type === 'function') {
        const name = value.name;
        if (!name) return '[Function (anonymous)]';
        return Function.prototype.toString.call(value).startsWith('class')
            ? '[class ' + name + ']' : '[Function: ' + name + ']';
    }

    // ---- 以下是对象 ----
    if (seen.indexOf(value) >= 0) return '[Circular]';

    const tag = Object.prototype.toString.call(value);
    if (depth < 0) {
        // 太深了: node 只给"类型名", 不再展开 (标量在任意深度都会打出来, 见上面)
        if (tag === '[object Array]') return '[Array]';
        return tag === '[object Object]' ? '[Object]' : '[' + tag.slice(8, -1) + ']';
    }
    if (tag === '[object Date]') return isNaN(value.getTime()) ? 'Invalid Date' : value.toISOString();
    if (tag === '[object RegExp]') return String(value);
    if (value instanceof Error || tag === '[object Error]') {
        // node 打的是**栈**, 首行是 `Name: message`; quickjs 的 stack 只有调用帧(实测) ⇒ 缺了就补上
        const head = (value.name || 'Error') + (value.message ? ': ' + value.message : '');
        if (typeof value.stack === 'string' && value.stack.length > 0)
            return value.stack.startsWith(head) ? value.stack : head + '\n' + value.stack;
        return head;
    }
    if (tag === '[object Promise]') return 'Promise { <unknown> }';
    if (tag === '[object WeakMap]') return 'WeakMap { <items unknown> }';
    if (tag === '[object WeakSet]') return 'WeakSet { <items unknown> }';
    if (tag === '[object ArrayBuffer]') return 'ArrayBuffer { byteLength: ' + value.byteLength + ' }';

    if (tag === '[object Map]' || tag === '[object Set]') {
        const isMap = (tag === '[object Map]');
        const parts = [];
        seen.push(value);
        for (const item of value) {
            parts.push(isMap
                ? __nodeInspect__(item[0], depth - 1, seen) + ' => ' + __nodeInspect__(item[1], depth - 1, seen)
                : __nodeInspect__(item, depth - 1, seen));
        }
        seen.pop();
        return __nodeWrapBody__((isMap ? 'Map' : 'Set') + '(' + value.size + ')', parts.join(', '));
    }
    if (ArrayBuffer.isView(value) && typeof value.length === 'number') {
        // TypedArray: node 打 `Uint8Array(3) [ 1, 2, 3 ]`
        const parts = [];
        seen.push(value);
        for (let i = 0; i < value.length; i++) parts.push(String(value[i]));
        seen.pop();
        return tag.slice(8, -1) + '(' + value.length + ') [ ' + parts.join(', ') + ' ]';
    }

    if (tag === '[object Array]') {
        const parts = [];
        seen.push(value);
        for (let i = 0; i < value.length; i++)
            parts.push((i in value) ? __nodeInspect__(value[i], depth - 1, seen) : '<1 empty item>');
        for (const key of Object.keys(value))          // 数组上的额外属性: node 附在末尾
            if (!/^(0|[1-9][0-9]*)$/.test(key))
                parts.push(__nodeInspectKey__(key) + ': ' + __nodeInspectMember__(value, key, depth - 1, seen));
        seen.pop();
        return parts.length > 0 ? '[ ' + parts.join(', ') + ' ]' : '[]';
    }

    // 普通对象 / 类实例
    const proto = Object.getPrototypeOf(value);
    const parts = [];
    seen.push(value);
    for (const key of Object.keys(value))
        parts.push(__nodeInspectKey__(key) + ': ' + __nodeInspectMember__(value, key, depth - 1, seen));
    for (const sym of Object.getOwnPropertySymbols(value)) {
        const d = Object.getOwnPropertyDescriptor(value, sym);
        if (d && d.enumerable)
            parts.push('[' + String(sym) + ']: ' +
                       ((d.get || d.set) ? '[Getter]' : __nodeInspect__(d.value, depth - 1, seen)));
    }
    seen.pop();

    let head = '';
    if (proto === null) head = '[Object: null prototype]';
    else {
        const ctor = proto.constructor;
        const name = ctor && ctor.name;
        if (name && name !== 'Object') head = name;    // 类实例: `Foo { a: 1 }`
    }
    const body = parts.join(', ');
    return head ? __nodeWrapBody__(head, body) : (body.length > 0 ? '{ ' + body + ' }' : '{}');
}

// console.log(...args) 的全部输出 (node 的 util.format 规则 + 顶层字符串不加引号):
//   · 一个字符串参数 -> 原样输出(不解析 %);  · 首参是字符串 -> 处理 %s %d %i %f %j %o %O %c %%,
//   用不完的参数照旧接在后面;  · 首参不是字符串 -> 各参数空格拼接, 顶层字符串不加引号。
//   ⚠ 认不出的说明符(如 %x)和"没有参数可填"的情况都原样保留(与 node 一致)。
function __nodeFormat__(args) {
    if (args.length === 0) return '';
    const first = args[0];
    if (args.length === 1) return (typeof first === 'string') ? first : __nodeInspect__(first);

    let out = '';
    let used = 0;
    if (typeof first === 'string') {
        used = 1;
        for (let p = 0; p < first.length; p++) {
            const ch = first[p];
            if (ch !== '%' || p + 1 >= first.length) { out += ch; continue; }
            const spec = first[p + 1];
            if (spec === '%') { out += '%'; p++; continue; }
            if ('sdifjoOc'.indexOf(spec) < 0 || used >= args.length) { out += ch; continue; }
            const v = args[used++];
            if (spec === 's') {
                // node: 对象(且没有自定义 toString)按 depth 0 inspect; BigInt 带 n
                out += (typeof v === 'bigint') ? String(v) + 'n'
                     : (v !== null && typeof v === 'object') ? __nodeInspect__(v, 0)
                     : String(v);
            }
            else if (spec === 'i') out += String(parseInt(v));
            else if (spec === 'd') out += String(Number(v));
            else if (spec === 'f') out += String(parseFloat(v));
            else if (spec === 'j') { try { out += JSON.stringify(v); } catch (e) { out += '[Circular]'; } }
            else if (spec === 'c') { /* CSS: 吃掉, 不输出 */ }
            else out += __nodeInspect__(v);            // %o / %O
            p++;
        }
    }
    for (; used < args.length; used++) {
        const v = args[used];
        out += (out.length > 0 ? ' ' : '') + ((typeof v === 'string') ? v : __nodeInspect__(v));
    }
    return out;
}

// console.assert(value, ...args): node 语义 —— 失败**只打印不抛异常**(Qt->JS 的移植代码把
// Q_ASSERT 一律译成 console.assert, 在这里抛异常会把"夹带断言检查的正常流程"整体炸掉);
// 消息的拼法也与 node 一致: 没消息 -> "Assertion failed"; 首参是字符串 -> 加前缀;
// 否则在最前面插一个 "Assertion failed"。
function __consoleAssert__(value, ...args) {
    if (value) return;
    if (args.length === 0) args = ['Assertion failed'];
    else if (typeof args[0] === 'string') args[0] = 'Assertion failed: ' + args[0];
    else args.unshift('Assertion failed');
    console.log(...args);        // console 对象由 C++ 侧稍后注册, 这里使用时才取
}
)";

    // 注册内部助手(全局空间执行, 与旧 loadScript 语义一致)。
    // loadScript 已废弃(无 import 支持的旧式全局 eval); 这类全局脚本片段用 eval() 即可。
    JZJSValuePtr ret_init = eval(init_code);
    assert(!JS_IsException(ret_init.get()));

    JS_SetRuntimeOpaque(m_rt, this);
    JS_SetContextOpaque(m_ctx, this);

    JSValue sleep_func = JS_NewCFunction(m_ctx, JSInter::js_sleep, "sleep", 1);
    JS_SetPropertyStr(m_ctx, m_global, "sleep", sleep_func);

    // 全局 self(): 取"当前正在被 JS 调用的对象"的包装 —— quickjs 没有取当前 this 的 API,
    // 由引擎维护调用栈实现(见 JZJSEngine::self 的说明)。典型用途: 箭头函数写的事件回调
    // (onPaint / onClicked ...) 里没有 this, 用 self() 拿回当前这个控件/对象。
    JSValue self_func = JS_NewCFunction(m_ctx, JSInter::js_self, "self", 0);
    JS_SetPropertyStr(m_ctx, m_global, "self", self_func);

    JSValue set_interval =
        JS_NewCFunctionMagic(m_ctx, JSInter::js_set_interval, "setInterval", 2, JS_CFUNC_generic_magic, 0);
    JS_SetPropertyStr(m_ctx, m_global, "setInterval", set_interval);

    JSValue set_timeout =
        JS_NewCFunctionMagic(m_ctx, JSInter::js_set_interval, "setInterval", 2, JS_CFUNC_generic_magic, 1);
    JS_SetPropertyStr(m_ctx, m_global, "setTimeout", set_timeout);

    JSValue clear_interval =
        JS_NewCFunctionMagic(m_ctx, JSInter::js_clear_interval, "clearInterval", 2, JS_CFUNC_generic_magic, 0);
    JS_SetPropertyStr(m_ctx, m_global, "clearInterval", clear_interval);

    JSValue clear_timeout =
        JS_NewCFunctionMagic(m_ctx, JSInter::js_clear_interval, "clearTimeout", 2, JS_CFUNC_generic_magic, 1);
    JS_SetPropertyStr(m_ctx, m_global, "clearTimeout", clear_timeout);

    // engine
    JSValue engine = JS_NewObject(m_ctx);

    JSValue interrup_func = JS_NewCFunction(m_ctx, JSInter::js_is_interruption, "isInterruption", 2);
    JS_SetPropertyStr(m_ctx, engine, "isInterruption", interrup_func);
    JS_SetPropertyStr(m_ctx, m_global, "engine", engine);

}

void JZJSEngine::registConsole()
{
    JSValue console = JS_NewObject(m_ctx);

    // console: log / error / warn 都是 Node 的 console 语义 —— 输出格式 = node 的 util.format
    // (见 print/nodeFormat 与 JS 侧 __nodeFormat__), 三个只走同一 sink(sigLog), 不区分级别。
    JSValue log_func = JS_NewCFunction(m_ctx, JSInter::js_console_log, "log", 1);
    JSValue error_func = JS_NewCFunction(m_ctx, JSInter::js_console_log, "error", 1);
    JSValue warn_func = JS_NewCFunction(m_ctx, JSInter::js_console_log, "warn", 1);
    // console.debugPrint: JZ 原来的紧凑格式(字符串带引号、`{a: 1}` 无内边距, 且能列出继承来的
    // 可枚举属性 —— 看 JZ 绑定对象时比 node 格式有信息量) ⇒ 旧输出依赖/想看全字段时用它。
    // (没叫 print 是因为 "print" 太泛, 也容易被当成 node 有的东西; dir/debug 在 node 里已有语义。)
    JSValue debug_print_func = JS_NewCFunction(m_ctx, JSInter::js_console_debug_print, "debugPrint", 1);
    // console.assert: 参数语义在 JS 侧(__consoleAssert__, 见 init_code), 那里用的是 node 的规则
    JSValue assert_func = JS_GetPropertyStr(m_ctx, m_global, "__consoleAssert__");
    JS_SetPropertyStr(m_ctx, console, "log", log_func);
    JS_SetPropertyStr(m_ctx, console, "error", error_func);
    JS_SetPropertyStr(m_ctx, console, "warn", warn_func);
    JS_SetPropertyStr(m_ctx, console, "debugPrint", debug_print_func);
    JS_SetPropertyStr(m_ctx, console, "assert", assert_func);
    JS_SetPropertyStr(m_ctx, m_global, "console", console);

    // process
    JSValue process = JS_NewObject(m_ctx);
    registFunction(process, "cwd", jsbind::createFunction([this]() -> string { return m_workDir; }));
    JS_SetPropertyStr(m_ctx, m_global, "process", process);

    // ---- process.exit / process.exitCode(对齐 Node) ----
    // 分两层:
    //   ① __jz.processRaw: C++ 侧的"原子操作"(只认已经校验过的整数, 不管参数形状);
    //   ② 下面那段 JS 引导: 按 Node 的规则校验参数, 建出 __jz.process, 并把它挂到全局 process 上。
    // 为什么要分: 参数规则(整数串/null/undefined/RangeError/TypeError)写 JS 里最清楚,
    // 也与 js_wrapper/process 共用**同一份实现**(避免两处校验各自漂移)。
    {
        JSValue raw = JS_NewObject(m_ctx);
        registFunction(raw, "exit", jsbind::createFunction([this](int code) { requestProcessExit(code); }));
        registFunction(raw, "exitCode", jsbind::createFunction([this]() -> JSValue {
            if (!m_processExitCodeSet)
                return JS_UNDEFINED;   // 没设过 = undefined(与 Node 一致)
            return JS_NewInt32(m_ctx, m_processExitCode);
        }));
        registFunction(raw, "setExitCode", jsbind::createFunction([this](int code) { setProcessExitCode(code); }));
        registFunction(raw, "clearExitCode", jsbind::createFunction([this]() { clearProcessExitCode(); }));

        // __jz: 与其它模块同一套 get-or-create(谁建的谁写回, 复用的要放掉 getProperty 多出来的引用)
        JSValue jz = JS_GetPropertyStr(m_ctx, m_global, "__jz");
        bool jzFromGet = !JS_IsUndefined(jz);
        if (!jzFromGet)
            jz = JS_NewObject(m_ctx);
        JS_SetPropertyStr(m_ctx, jz, "processRaw", raw);
        if (jzFromGet)
            JS_FreeValue(m_ctx, jz);   // 复用别人建的: 放掉这份多出来的引用(见 JZJSCoreModule 的说明)
        else
            JS_SetPropertyStr(m_ctx, m_global, "__jz", jz);   // 自己建的才写回(这份引用被 set 消费掉)
    }

    // 参数校验 + 挂到全局 process(规则与取值都逐条对着真 Node 校过, 见 build/_codex_bench/api_parity/exit_sem.py)
    eval(R"(
(function () {
    const raw = __jz.processRaw;
    // Node: 非整数数值 -> RangeError; 其它非(整数串/整数) 类型 -> TypeError; 两者都带 .code
    function typeError(what, v) {
        let got = (v === null) ? 'null' : (typeof v);
        if (got === 'object') got = 'an instance of Object';
        else if (got !== 'string') got = got + ' (' + String(v) + ')';
        const e = new TypeError('The "' + what + '" argument must be of type number. Received ' + got);
        e.code = 'ERR_INVALID_ARG_TYPE';
        throw e;
    }
    function rangeError(what, v) {
        const e = new RangeError('The value of "' + what + '" is out of range. It must be an integer. Received ' + v);
        e.code = 'ERR_OUT_OF_RANGE';
        throw e;
    }
    function toInt(v, what) {
        if (typeof v === 'number') {
            if (!Number.isInteger(v)) rangeError(what, v);
            return v;
        }
        if (typeof v === 'string' && /^[+-]?\d+$/.test(v.trim())) return parseInt(v.trim(), 10);
        typeError(what, v);
    }

    const api = {
        // exit(): 省略/null 时用 exitCode(设过)否则 0 —— 与 Node 一致
        exit: function (code) {
            let n;
            if (code === undefined || code === null) {
                const cur = raw.exitCode();
                n = (cur === undefined) ? 0 : cur;
            } else {
                n = toInt(code, 'code');
            }
            raw.exit(n);      // 记退出码 + 请求宿主退出(主引擎: jzApp->exit(n))
        },
        exitCode: function () { return raw.exitCode(); },
        setExitCode: function (v) {
            if (v === undefined || v === null) { raw.clearExitCode(); return; }
            raw.setExitCode(toInt(v, 'code'));
        },
    };
    __jz.process = api;

    // 全局 process(engine 注册的最小对象): Node 的脚本习惯直接用 process.exit, 这里补上
    const p = globalThis.process;
    if (p) {
        p.exit = api.exit;
        Object.defineProperty(p, 'exitCode', {
            enumerable: true, configurable: true,
            get: function () { return api.exitCode(); },
            set: function (v) { api.setExitCode(v); },
        });
    }
})();
)");

    // fs
    JSValue read_str_func = JS_NewCFunction(m_ctx, JSInter::js_fs_readString, "readString", 1);

    JSValue fs = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, fs, "readString", read_str_func);
    JS_SetPropertyStr(m_ctx, m_global, "fs", fs);
}

std::string JZJSEngine::workDir()
{
    return m_workDir;
}

std::string JZJSEngine::fullPath(std::string& path)
{
    JZDir dir(m_workDir);
    return dir.absoluteFilePath(path);
}

int JZJSEngine::status()
{
    return m_status;
}

void JZJSEngine::setStatus(JZJSEngineStatus status)
{
    if (m_status != status)
    {
        assert((m_status == Engine_none && (status == Engine_pause || status == Engine_running)) ||
               (m_status == Engine_running && (status == Engine_pause || status == Engine_error || status == Engine_none)) ||
               (m_status == Engine_pause && (status == Engine_running || status == Engine_error || status == Engine_none)) ||
               (m_status == Engine_error && status == Engine_none));

        m_status = status;
        sigStatus.emit(status);
    }
}

void JZJSEngine::start()
{
    startEngineTimer();
    setStatus(Engine_running);
}

void JZJSEngine::stop()
{
    if (!m_ctx)
        return;

    stopEngineTimer();
    setStatus(Engine_none);
}

void JZJSEngine::pause()
{
    assert(m_ctx);
    stopEngineTimer();
    setStatus(Engine_pause);
}

void JZJSEngine::resetUncaughtError()
{
    JS_ResetUncatchableError(m_ctx);
    startEngineTimer();   // onExceptionOccurred 已 stopEngineTimer, 必须重启, 否则 execOnce 永不再触发
    setStatus(Engine_pause);
}

void JZJSEngine::resume()
{
    assert(m_ctx && m_status == Engine_pause);
    startEngineTimer();
    setStatus(Engine_running);
}

void JZJSEngine::startEngineTimer()
{
    // 事件驱动下"恢复/继续驱动": 按最近到期任务挂一次性 timer;
    // 没有任务就不挂(空闲停表), 任务到来时由 setInterval / wakeUp 等入口负责重新唤醒。
    scheduleNextTimer();
}

void JZJSEngine::stopEngineTimer()
{
    if (m_timer != INVALID_TIMER_ID)
    {
        killTimer(m_timer);
        m_timer = INVALID_TIMER_ID;
        m_timerExpire = 0;
    }
}

// 按"最近的 JS 定时任务到期时刻"挂一次性 timer。引擎线程内调用(startTimer 断言当前线程)。
// 已在跑的 timer 只要不晚于最近到期时刻就不动(避免反复 kill/start), 到点后 execOnce
// 会处理全部到期任务并再来这里续期; 新任务比已挂 timer 更早到期时才重挂到更早时刻。
void JZJSEngine::scheduleNextTimer()
{
    if (!m_ctx)
        return;

    int64_t min_next = 0;
    bool has_task = false;
    for (int i = 0; i < (int)m_timeTasks.size(); i++)
    {
        if (!has_task || m_timeTasks[i].next_run_time < min_next)
        {
            has_task = true;
            min_next = m_timeTasks[i].next_run_time;
        }
    }
    if (!has_task)
        return;     // 没有 JS 定时任务: 保持停表(空闲零唤醒)

    if (m_timer != INVALID_TIMER_ID && m_timerExpire <= min_next)
        return;     // 已挂 timer 会先(或同时)到期, 沿用即可

    if (m_timer != INVALID_TIMER_ID)
    {
        killTimer(m_timer);     // 新任务更早到期, 重挂到更早时刻
        m_timer = INVALID_TIMER_ID;
    }

    int64_t delta = min_next - currentMs();
    if (delta < 1)
        delta = 1;              // 已到期/立即触发(setTimeout 0): 尽快触发, 宿主 timer 最小间隔兜底
    m_timer = startTimer((int)delta);
    m_timerExpire = min_next;
}

// 线程安全唤醒: 任意线程(C++ promise 结算 / 协程 / 工作线程)都能调用。
// 投递自定义事件到引擎线程, customEvent -> execOnce 泵一轮(promise job / 到期任务)。
void JZJSEngine::wakeUp()
{
    if (!m_ctx || !jzApp)
        return;
    jzApp->postEvent(this, new JZEvent(JZEngine_WakeUp));
}

void JZJSEngine::wakeUpIfJobPending()
{
    if (m_ctx && JS_IsJobPending(m_rt))
        wakeUp();
}

// C++ 主动调用 JS 的唯一入口(见头文件说明): JS_Call + 一次微任务检查点。
JSValue JZJSEngine::callJs(JSValueConst func_obj, JSValueConst this_obj, int argc, JSValueConst* argv)
{
    if (!m_ctx)
        return JS_EXCEPTION;   // 引擎已销毁(收尾期): 调用方自己也不该在没有 ctx 时进来

    JSValue ret = JS_Call(m_ctx, func_obj, this_obj, argc, argv);

    // 回调里可能: ① resolve 了别人 await 的 promise; ② 自己是个 async 函数跑到 await 就返回;
    // ③ .then/.catch/.finally 排了新的续体 —— 都是 job, 必须让 execOnce 泵一轮(有才唤醒)。
    wakeUpIfJobPending();

    return ret;
}

void JZJSEngine::timerEvent(JZTimerEvent* event)
{
    if (event->id() == m_timer)
    {
        // 一次性到期: 宿主 JZThread 定时器到点会周期续约, 先 kill 停掉续约;
        // 是否再挂、挂到几时由 execOnce 尾部的 scheduleNextTimer 按剩余任务决定。
        killTimer(m_timer);
        m_timer = INVALID_TIMER_ID;
        m_timerExpire = 0;
    }

    execOnce();
}

void JZJSEngine::customEvent(JZEvent* event)
{
    // ---- worker 事件(与引擎运行状态无关: 引擎可能已 error/pause, 但消息与线程回收不能停) ----
    // 消息: 投给"目标 worker 的引擎"(本对象), 转交给本引擎的 worker 模块
    if (event->type() == JZJSWorkerEvent_Message)
    {
        JZJSWorkerMessageEvent* e = (JZJSWorkerMessageEvent*)event;
        if (m_workerModule)
            m_workerModule->onMessageEvent(e->fromId, e->data);
        return;
    }
    // 我名下的某个 worker 退出了: 交给模块跑 JS 侧 'exit' 回调
    if (event->type() == JZJSWorkerEvent_Exit)
    {
        JZJSWorkerExitEvent* e = (JZJSWorkerExitEvent*)event;
        if (m_workerModule)
            m_workerModule->onWorkerExit(e->workerId, e->code);
        return;
    }

    // 暂停/停止/错误态不处理残留唤醒(与旧"停表"语义一致); 任务在 resume/start 时恢复调度
    if (m_status != Engine_running)
        return;

    if (event->type() == JZEngine_WakeUp)
        execOnce();
}

int64_t JZJSEngine::currentMs()
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::string JZJSEngine::getPropertyStr(JSValue var, std::string name)
{
    std::string ret;

    JSValue prop = JS_GetPropertyStr(m_ctx, var, name.c_str());
    if (JS_IsException(prop))
    {
        // 取值本身就抛异常(var 是 null/undefined, 或 getter 抛错): 必须吞掉,
        // 否则异常残留到下一次 JS 调用里, 错误定位会彻底跑偏。
        JS_FreeValue(m_ctx, JS_GetException(m_ctx));
        return ret;
    }

    // 属性不存在(undefined/null)返回空串, 便于调用方区分"没有这个属性"
    if (!JS_IsUndefined(prop) && !JS_IsNull(prop))
        ret = jsValueToStr(prop);

    JS_FreeValue(m_ctx, prop);   // prop 也是 JS_GetPropertyStr 返回的引用, 必须释放

    return ret;
}

std::string JZJSEngine::jsValueToStr(JSValue value)
{
    const char* text = JS_ToCString(m_ctx, value);
    if (!text)
    {
        // symbol 等无法转字符串, 且失败会留下待处理异常
        JS_FreeValue(m_ctx, JS_GetException(m_ctx));
        return std::string();
    }

    std::string ret = text;
    JS_FreeCString(m_ctx, text);
    return ret;
}

bool JZJSEngine::isException(JSValue value)
{
    return JS_IsException(value);
}

std::string JZJSEngine::getExceptionDescript(JSValue exc)
{
    // 异常已被别人取走时 JS_GetException 返回 JS_UNINITIALIZED; 正常情况下不应到这里 ——
    // 出现说明有代码 return JS_EXCEPTION 却没先 JS_Throw*(见 call() 里的 JZ_ASSERT_X 保护)
    if (JS_IsUninitialized(exc))
        return "<no pending exception: return JS_EXCEPTION 但没有 JS_Throw*>";
    if (JS_IsUndefined(exc))
        return "<undefined>";
    if (JS_IsNull(exc))
        return "<null>";

    // throw "str" / throw 123 / throw {...}: 这种抛出物没有 message / stack,
    // 直接读属性只会得到 "undefined\nundefined" —— 按抛出物本身转文本。
    if (!JS_IsError(exc))
    {
        if (JS_IsObject(exc))
        {
            std::string text = printObject(exc);
            if (!text.empty())
                return text;
        }
        return jsValueToStr(exc);
    }

    std::string name = getPropertyStr(exc, "name");
    std::string msg = getPropertyStr(exc, "message");
    std::string stack = getPropertyStr(exc, "stack");

    std::string error = name.empty() ? msg : name + ": " + msg;
    if (!stack.empty())
        error += "\n" + stack;

    return error;
}

void JZJSEngine::unHandleException()
{
    JSValue exc = JS_GetException(m_ctx);
    std::string runtimeError = getExceptionDescript(exc);
    JS_FreeValue(m_ctx, exc);

    onExceptionOccurred(runtimeError);
}

void JZJSEngine::unHandlePromiseReject(JSValueConst promise, JSValueConst reason)
{
    std::string err;
    if (JS_IsException(reason))
    {
        JSValue exc = JS_GetException(m_ctx);
        err = getExceptionDescript(exc);   // 保留真实异常文本, 而非写死的 "unHandle Exception"
        JS_FreeValue(m_ctx, exc);
    }
    else if (JS_IsError(reason))
        err = getExceptionDescript(reason);
    else
    {
        err = printObject(reason);
    }

    // 一条一条记, 每条都带上产生它的 promise(便于稍后挂 handler 时撤掉)。
    // 多个未处理拒绝在 takePromiseRejectText 里按换行累积, 避免只保留最后一个。
    PendingReject pr;
    pr.promise = JS_IsUndefined(promise) ? JS_UNDEFINED : JS_DupValue(m_ctx, promise);
    pr.text = err;
    m_promiseRejects.push_back(std::move(pr));
}

// quickjs: 给"已经拒绝且还没人管"的 promise 挂上了 handler ⇒ 这条待报拒绝作废
// (常见写法 Promise.reject(e).catch(...) / async 函数里抛错后由调用方 catch)
void JZJSEngine::promiseRejectHandled(JSValueConst promise)
{
    if (!m_ctx || JS_IsUndefined(promise))
        return;

    for (size_t i = 0; i < m_promiseRejects.size(); i++)
    {
        JSValue cur = m_promiseRejects[i].promise;
        if (JS_IsUndefined(cur))
            continue;   // native 任务那条, 没有 promise 可匹配
        if (JS_IsStrictEqual(m_ctx, cur, promise))
        {
            JS_FreeValue(m_ctx, cur);
            m_promiseRejects.erase(m_promiseRejects.begin() + i);
            return;
        }
    }
}

std::string JZJSEngine::takePromiseRejectText()
{
    std::string text;
    for (size_t i = 0; i < m_promiseRejects.size(); i++)
    {
        if (!text.empty())
            text += "\n";
        text += m_promiseRejects[i].text;
    }
    clearPromiseRejects();
    return text;
}

void JZJSEngine::clearPromiseRejects()
{
    if (m_ctx)
    {
        for (size_t i = 0; i < m_promiseRejects.size(); i++)
        {
            if (!JS_IsUndefined(m_promiseRejects[i].promise))
                JS_FreeValue(m_ctx, m_promiseRejects[i].promise);
        }
    }
    m_promiseRejects.clear();
}

void JZJSEngine::asyncResultResolve(int handle, int argc, JSValueConst* argv)
{
    if (!m_ctx)
        return;

    JZAsyncResult ar;
    ar.result = handle;
    for (int i = 0; i < argc; i++)
    {
        // argv 属于 promise 结算帧, 回调返回后由 quickjs 释放, 这里必须 dup 持有
        ar.params.push_back(JZJSValuePtr(m_ctx, JS_DupValue(m_ctx, argv[i])));
    }
    sigAsyncResult.emit(ar);
}

void JZJSEngine::asyncResultReject(int handle, int argc, JSValueConst* argv)
{
    // 复用"未处理拒绝"上报通道: 错误文本累积进 m_promiseError,
    // 由 pumpPendingJobs 在 job 泵结束统一转 Engine_error/sigError(与 JS 侧未 catch 的 reject 一致)
    if (argc > 0)
        unHandlePromiseReject(JS_UNDEFINED, argv[0]);
}

JZJSEngine::PromiseTask* JZJSEngine::getPromiseTask(int handle)
{
    auto it = m_promiseTask.find(handle);
    if (it == m_promiseTask.end())
        return nullptr;

    return it->second.get();
}

void JZJSEngine::updateFunction(shared_ptr<CFunction> func)
{
    if (!func)   // 只读属性的 write 为空
        return;

    for (int i = 0; i < (int)func->params.size(); i++)
    {
        // 前向引用(如 JZWidget<->JZLayout 互相引用)时对应类尚未注册, 此处 ctypeidToType 为
        // JSType_none; 实际调用时所有类均已注册, 改由 call()/callAsync() 按 ctypeid 惰性解析。
        func->params[i].type = ctypeidToType(func->params[i].ctypeid);
    }

    if (!func->result.ctypeid.empty())
    {
        func->result.type = ctypeidToType(func->result.ctypeid);
    }
}

void JZJSEngine::registFunction(JSValue this_val, std::string name, std::shared_ptr<CFunction> func)
{
    updateFunction(func);

    auto js_func = JS_NewCFunctionMagic(m_ctx, JSInter::js_cfunction, name.c_str(), (int)func->params.size(),
                                        JS_CFUNC_generic_magic, (int)m_funcList.size());
    JS_SetPropertyStr(m_ctx, this_val, name.c_str(), js_func);

    m_funcList.push_back(func);
}

void JZJSEngine::registAsyncFunction(JSValue this_val, string name, shared_ptr<CFunction> func)
{
    updateFunction(func);
    JZ_ASSERT(func->params.size() > 0 && func->params[0].type == JSType_int);

    auto js_func = JS_NewCFunctionMagic(m_ctx, JSInter::js_async_cfunction, name.c_str(), (int)func->params.size() - 1,
                                        JS_CFUNC_generic_magic, (int)m_funcList.size());
    JS_SetPropertyStr(m_ctx, this_val, name.c_str(), js_func);

    m_funcList.push_back(func);
}

void JZJSEngine::registType(TypeInfo type_info)
{
    // 同一 type 重复注册会把 m_typeMap 里那份"名字/定义的宿主"整体换掉 —— 而 JS 侧此刻已经
    // 拿着指向它内部字符串的裸指针(JSCFunctionListEntry::name / JSClassDef::class_name,
    // 见 registClass 的说明), 换掉就是悬垂。故这里保留首次注册(不止是调试期 assert)。
    if (m_typeMap.find(type_info.type) != m_typeMap.end())
    {
        assert(false && "registType: 同一 type 重复注册(会破坏已注册名字的地址稳定性)");
        return;
    }
    m_typeMap[type_info.type] = type_info;

    m_jsTypeMap[type_info.jsType] = type_info.type;
    m_cTypeMap[type_info.ctypeId] = type_info.type;
}

void JZJSEngine::registBase(string name, int type, int js_type, string ctypeid)
{
    TypeInfo type_info;
    type_info.name = name;
    type_info.type = type;
    type_info.jsType = js_type;
    type_info.ctypeId = ctypeid;
    registType(type_info);
}

void JZJSEngine::registClass(const CClassDefine& define, int define_id)
{
    JSValue js_proto, js_class;

    int object_id;
    if (define_id != JSType_none)
        object_id = define_id;
    else
        object_id = m_objectId++;

    // 同一 type 只能注册一次: 名字/定义都存进 m_typeMap 那份副本, 而 quickjs 已按首次注册时
    // 发出的裸指针在用它们 —— 重复注册要么换掉宿主(悬垂), 要么让下面 stored 与 define 尺寸对不上。
    if (m_typeMap.find(object_id) != m_typeMap.end())
    {
        assert(false && "registClass: 同一 type 重复注册(名字宿主不能换)");
        return;
    }

    // ⚠ 类 id 必须由 quickjs 发号, 别拿引擎的"类型号"当 classID: 两者是**两套号段**
    // (类型号从 JSType_objectUser 起, quickjs 的 classID 由 JS_NewClassID 全局递增发号),
    // 混用就会与其它用 JS_NewClassID 的模块(控件族的 op 类等)撞号。撞号后 jsValueType()
    // 会把控件对象当成某个引擎类 -> 形参校验/转换时好时坏(2026-09-18 抓到的偶发崩就是它:
    // 同一个 JZWindow 有时 checkType(v,"JZWidget")==false, js_JZLabel_ctor 进而抛"参数不匹配")。
    // 两套号靠 m_jsTypeMap 关联(见 registType / jsTypeToType)。
    JSClassID js_class_id = 0;
    JS_NewClassID(m_rt, &js_class_id);

    auto funcList = std::make_shared<vector<JSCFunctionListEntry>>();

    TypeInfo info;
    info.name = define.name;
    info.type = object_id;
    info.ctypeId = define.ctypeid;
    info.jsType = js_class_id;
    info.classDefine = define;
    info.superCTypeId = define.superCTypeid;
    info.funcList = funcList;
    registType(info);

    // ---- 名字的宿主: m_typeMap 里那份 TypeInfo::classDefine ----
    // quickjs 只存名字的**裸指针**(JSClassDef::class_name 存进 JSClass; JSCFunctionListEntry::name
    // 存进属性表, 且首次访问该属性才懒实例化 —— 那时才读 name), 所以这些指针必须长期有效。
    // 宿主就用 registType 刚存进去的那份**引擎自己的副本**: std::map 节点地址稳定, 而且 registType
    // 已拒绝同 type 覆盖 -> 永不失效。**别**改成用参数 define 的字段或任何临时字符串: 它往往是调用方
    // 栈上的局部 ClassBind, registClass 一返回就析构(这里曾因此出过 heap-use-after-free)。
    // 注: define.functions(def 注册的方法)的名字宿主是 m_funcList 里那份 shared_ptr<CFunction>。
    const CClassDefine& stored = m_typeMap[object_id].classDefine;

    JSClassDef js_define = {};
    js_define.class_name = stored.name.c_str();
    js_define.finalizer = JSInter::js_deconstructor;
    JS_NewClass(m_rt, js_class_id, &js_define);

    for (int i = 0; i < (int)define.propertys.size(); i++)
    {
        auto& prop = define.propertys[i];
        updateFunction(prop.read);
        updateFunction(prop.write);
        // 原型上的属性访问器 = 类实例方法(this = 实例) -> 进"当前 this"栈, 见 CFunction::bindInstance
        if (prop.read)
            prop.read->memberFunction = true;
        if (prop.write)
            prop.write->memberFunction = true;

        int magic = (int)m_funcList.size();

        JSCFunctionListEntry entry = {};
        // 名字宿主 = m_typeMap 里那份副本(见 registClass 顶部说明); 必须长期有效, 懒实例化时才读
        entry.name = stored.propertys[i].name.c_str();
        entry.def_type = JS_DEF_CGETSET_MAGIC;
        entry.magic = magic;

        m_funcList.push_back(prop.read);
        entry.u.getset.get.getter_magic = JSInter::js_property_get;

        if (!prop.write)
            entry.prop_flags = JS_PROP_ENUMERABLE;
        else
        {
            entry.prop_flags = JS_PROP_WRITABLE | JS_PROP_ENUMERABLE;
            m_funcList.push_back(prop.write);
            entry.u.getset.set.setter_magic = JSInter::js_property_set;
        }

        funcList->push_back(entry);
    }
    for (int i = 0; i < (int)define.functions.size(); i++)
    {
        auto& func = define.functions[i];
        updateFunction(func);
        // 原型方法 = 类实例方法(this = 实例) -> 进"当前 this"栈, 见 CFunction::bindInstance
        if (func)
            func->memberFunction = true;

        int magic = (int)m_funcList.size();
        m_funcList.push_back(func);

        JSCFunctionListEntry entry = {};
        entry.name = func->name.c_str();
        entry.def_type = JS_DEF_CFUNC;
        entry.prop_flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
        entry.magic = magic;
        entry.u.func.length = (uint8_t)func->params.size();
        entry.u.func.cproto = JS_CFUNC_generic_magic;
        entry.u.func.cfunc.generic_magic = JSInter::js_member_function;

        funcList->push_back(entry);
    }
    // 直接用 C 函数注册的方法(ClassBind::def(name, JSCFunction*)): 不进 magic 表, 交给 quickjs
    // 以 JS_CFUNC_generic **原样**调用 —— 参数校验 / this_val 由该 C 函数自己处理。
    for (int i = 0; i < (int)define.rawFunctions.size(); i++)
    {
        const CRawFunction& raw = define.rawFunctions[i];

        JSCFunctionListEntry entry = {};
        // 名字宿主 = m_typeMap 里那份副本的 rawFunctions(裸函数同样是 CClassDefine 的成员)
        entry.name = stored.rawFunctions[i].name.c_str();
        entry.def_type = JS_DEF_CFUNC;
        entry.prop_flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
        entry.u.func.length = (uint8_t)raw.length;
        entry.u.func.cproto = JS_CFUNC_generic;
        entry.u.func.cfunc.generic = raw.function;

        funcList->push_back(entry);
    }
    js_proto = JS_NewObject(m_ctx);
    JS_SetPropertyFunctionList(m_ctx, js_proto, funcList->data(), (int)funcList->size());

    // 带 func_data 的 C 函数(ClassBind::defData): 属性表里没有数据槽, 只能自己造函数对象再挂上去 ——
    // JS_NewCFunctionData2 会把 data 各 dup 一份存进函数对象(GC 负责 mark/free), 调用时原样交给
    // 回调的第 6 个参数。属性形状与其它方法一致: 可写 / 可配 / 不可枚举。
    for (int i = 0; i < (int)define.dataFunctions.size(); i++)
    {
        const CDataFunction& dfn = define.dataFunctions[i];
        // quickjs 只会把这些值各 dup 一份(s->data[i] = js_dup(data[i])), 故 const_cast 安全
        JSValue* data = dfn.data.empty() ? nullptr : const_cast<JSValue*>(dfn.data.data());
        // 这里名字是**立即**用掉的(建函数对象 + 挂属性都当场把名字转成 atom), 不存裸指针,
        // 但仍统一取存储副本的字段, 免得以后有人把它改成懒注册。
        const std::string& dfn_name = stored.dataFunctions[i].name;
        JSValue fn = JS_NewCFunctionData2(m_ctx, dfn.function, dfn_name.c_str(), dfn.length, dfn.magic,
                                          (int)dfn.data.size(), data);
        if (JS_IsException(fn))
            continue;
        JS_DefinePropertyValueStr(m_ctx, js_proto, dfn_name.c_str(), fn, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }

    js_class = JS_NewCFunctionMagic(m_ctx, JSInter::js_constructor, define.name.c_str(), 0, JS_CFUNC_constructor_magic,
                                    object_id);
    JS_SetConstructor(m_ctx, js_class, js_proto);

    // 继承: 把派生类原型挂到基类原型上, 直接复用 JS 原生原型链。
    // 这样 JS 侧 instanceof(基类)、基类方法在派生对象上查找、属性覆盖都按 JS 语义生效。
    // 单继承下基类子对象偏移为 0, 派生对象指针即基类指针, 无需 C++ 侧做指针向上转型。
    // 注意: JS_SetConstructor 会把 js_proto 的 [[Prototype]] 设为 js_class 本身, 故这里必须在它之后覆盖。
    if (!define.superCTypeid.empty())
    {
        int base_type = ctypeidToType(define.superCTypeid);
        assert(base_type != JSType_none && "registClass: 基类须先于派生类注册");
        if (base_type != JSType_none)
        {
            JSClassID base_js_class_id = typeToJSType(base_type);
            JSValue base_proto = JS_GetClassProto(m_ctx, base_js_class_id);
            JS_SetPrototype(m_ctx, js_proto, base_proto);
            JS_FreeValue(m_ctx, base_proto);
        }
    }

    JS_SetClassProto(m_ctx, js_class_id, js_proto);

    JS_SetPropertyStr(m_ctx, m_global, define.name.c_str(), js_class);
}

int JZJSEngine::setInterval(JSValue callback, int64_t interval, bool once)
{
    TimerTask task;
    task.runOnce = once;
    task.refed = true;      // unref() 才会关掉(见 setTimerRef)
    task.callback = JS_DupValue(m_ctx, callback);
    task.interval = interval;
    task.next_run_time = currentMs() + interval;
    task.id = m_timeTaskId++;

    m_timeTasks.push_back(task);

    // 新任务可能比已挂 timer 更早到期, 让调度器更新到最近到期时刻;
    // 暂停/停止态不驱动(任务冻结, 由 resume/start 恢复调度)
    if (m_status == Engine_running)
        scheduleNextTimer();

    return task.id;
}

void JZJSEngine::clearInterval(int id)
{
    int idx = -1;
    for (int i = 0; i < m_timeTasks.size(); i++)
    {
        if (m_timeTasks[i].id == id)
        {
            idx = i;
            break;
        }
    }
    if (idx == -1)
        return;

    JS_FreeValue(m_ctx, m_timeTasks[idx].callback);
    vec_removeAt(m_timeTasks, idx);
}

void JZJSEngine::setTimerRef(uint32_t id, bool refed)
{
    for (int i = 0; i < (int)m_timeTasks.size(); i++)
    {
        if (m_timeTasks[i].id == id)
        {
            m_timeTasks[i].refed = refed;
            return;
        }
    }
}

bool JZJSEngine::timerHasRef(uint32_t id) const
{
    for (int i = 0; i < (int)m_timeTasks.size(); i++)
    {
        if (m_timeTasks[i].id == id)
            return m_timeTasks[i].refed;
    }
    return false;   // 任务已经没了: 与 Node 的 hasRef() 一致(不存在的 handle 视为 false)
}

// Node 的 Timeout.refresh(): 从"现在"重新计时; 不给间隔就沿用原来的。
void JZJSEngine::refreshTimer(uint32_t id, int64_t interval)
{
    for (int i = 0; i < (int)m_timeTasks.size(); i++)
    {
        if (m_timeTasks[i].id != id)
            continue;

        if (interval > 0)
            m_timeTasks[i].interval = interval;
        m_timeTasks[i].next_run_time = currentMs() + m_timeTasks[i].interval;
        scheduleNextTimer();   // 可能比已挂的宿主 timer 更早到期
        return;
    }
}

string JZJSEngine::typeName(int type)
{
    auto it = m_typeMap.find(type);
    if (it == m_typeMap.end())
        return "none";

    return it->second.name;
}

void JZJSEngine::print(JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    sigLog.emit(nodeFormat(argc, argv));
}

// console.debugPrint: JZ 原来的紧凑格式 —— 每个参数过一遍 __printObject__(它自带引号/紧凑括号),
// 参数之间空格拼接。与 console.log 的差别见 init_code 里两段打印器的注释。
void JZJSEngine::debugPrint(JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    std::string log;
    for (int i = 0; i < argc; i++)
    {
        if (i > 0)
            log += " ";
        log += printObject(argv[i]);
    }
    sigLog.emit(log);
}

// console.* 的输出文本: 交给 JS 侧的 __nodeFormat__(node 的 util.format + util.inspect 规则)。
// 放在 JS 侧是因为要按类型分派(Date/Map/Set/RegExp/TypedArray/getter/symbol 键...), C++ 写一遍
// 既长又容易与 JS 语义(原型、Symbol.toStringTag、访问器属性)对不上。
string JZJSEngine::nodeFormat(int argc, JSValueConst* argv)
{
    JSValue args = JS_NewArray(m_ctx);
    for (int i = 0; i < argc; i++)
        JS_SetPropertyUint32(m_ctx, args, (uint32_t)i, JS_DupValue(m_ctx, argv[i]));   // 传进去的是副本

    JSValue fmt_func = JS_GetPropertyStr(m_ctx, m_global, "__nodeFormat__");
    JSValue result = callJs(fmt_func, JS_NULL, 1, &args);
    JS_FreeValue(m_ctx, fmt_func);
    JS_FreeValue(m_ctx, args);

    string str;
    if (JS_IsException(result))
    {
        // 格式化本身抛错(getter 抛错 / __nodeFormat__ 缺失): 吞掉异常, 退化成逐参数 toString
        JS_FreeValue(m_ctx, JS_GetException(m_ctx));
        for (int i = 0; i < argc; i++)
        {
            if (i > 0)
                str += " ";
            str += jsValueToStr(argv[i]);
        }
    }
    else
        str = ::fromJSValue<string>(m_ctx, result);
    JS_FreeValue(m_ctx, result);

    return str;
}

string JZJSEngine::printObject(JSValue value)
{
    JSValue print_func = JS_GetPropertyStr(m_ctx, m_global, "__printObject__");
    JSValue argv[] = {value};
    JSValue result = callJs(print_func, JS_NULL, 1, argv);
    JS_FreeValue(m_ctx, print_func);

    string str;
    if (JS_IsException(result))
    {
        // 打印过程中抛错(getter 抛错 / __printObject__ 缺失): 吞掉异常, 回退到默认转换
        JS_FreeValue(m_ctx, JS_GetException(m_ctx));
        str = jsValueToStr(value);
    }
    else
        str = ::fromJSValue<string>(m_ctx, result);
    JS_FreeValue(m_ctx, result);

    return str;
}

bool JZJSEngine::isVectorType(int type)
{
    auto it = m_typeMap.find(type);
    // 查不到就是"没这个类型"(见 checkType 的说明) —— 以前这里 assert + 直接解 end(), Release 下
    // 是 UB(读出来的 valueType 随机), 会让同一个值时而当 vector 时而当别的。
    if (it == m_typeMap.end())
        return false;

    return (it->second.valueType != JSType_none);
}

int JZJSEngine::ctypeidToType(string ctypeid)
{
    if (ctypeid.size() == 0)
        return JSType_none;

    if (ctypeid.back() == '*')
        ctypeid = ctypeid.substr(0, ctypeid.size() - 1);

    auto it = m_cTypeMap.find(ctypeid);
    if (it == m_cTypeMap.end())
        return JSType_none;

    return it->second;
}

int JZJSEngine::typeNameToType(std::string type_name)
{
    if (type_name.size() == 0)
        return JSType_none;

    auto it = m_typeMap.begin();
    while (it != m_typeMap.end())
    {
        if (it->second.name == type_name)
            return it->first;

        it++;
    }
    return JSType_none;
}

int JZJSEngine::jsTypeToType(int js_type)
{
    auto it = m_jsTypeMap.find(js_type);
    if (it == m_jsTypeMap.end())
        return JSType_none;

    return it->second;
}

int JZJSEngine::typeToJSType(int type)
{
    auto it = m_typeMap.find(type);
    if (it != m_typeMap.end())
        return it->second.jsType;

    return JS_TAG_UNDEFINED;
}

int JZJSEngine::jsValueType(JSValue v)
{
    int js_type = JS_VALUE_GET_TAG(v);
    if (js_type == JS_TAG_OBJECT)
    {
        js_type = JS_GetClassID(v);
        if (js_type == 1)  // JS_CLASS_OBJECT
            return JSType_jsonValue;
    }
    return jsTypeToType(js_type);
}

std::string JZJSEngine::classCTypeId(JSClassID id) const
{
    auto it = m_jsTypeMap.find((int)id);
    if (it == m_jsTypeMap.end())
        return "";
    auto tit = m_typeMap.find(it->second);
    if (tit == m_typeMap.end())
        return "";
    return tit->second.ctypeId;
}

bool JZJSEngine::isInstanceOf(const std::string& derivedCTypeId, const std::string& baseCTypeId) const
{
    std::string cur = derivedCTypeId;
    while (!cur.empty())
    {
        if (cur == baseCTypeId)
            return true;
        auto it = m_cTypeMap.find(cur);
        if (it == m_cTypeMap.end())
            return false;
        auto tit = m_typeMap.find(it->second);
        if (tit == m_typeMap.end())
            return false;
        cur = tit->second.superCTypeId;
    }
    return false;
}

JSValue JZJSEngine::classConstructor(int type) const
{
    auto it = m_typeMap.find(type);
    if (it == m_typeMap.end())
        return JS_UNDEFINED;
    return JS_GetPropertyStr(m_ctx, m_global, it->second.name.c_str());
}

bool JZJSEngine::canConvert(int from_type, int to_type)
{
    if (from_type == to_type)
        return true;

    if (from_type >= JSType_bool && from_type <= JSType_double && to_type >= JSType_bool && to_type <= JSType_double)
        return true;

    // 继承: 派生类对象可传给基类形参
    if (from_type >= JSType_object && to_type >= JSType_object)
    {
        auto fit = m_typeMap.find(from_type);
        auto tit = m_typeMap.find(to_type);
        if (fit != m_typeMap.end() && tit != m_typeMap.end())
            return isInstanceOf(fit->second.ctypeId, tit->second.ctypeId);
    }

    return false;
}

bool JZJSEngine::checkType(JSValue value, int type)
{
    // JSType_none = 没解析到类型(通常是"类名不在类型表里")。这种形参**不做校验**:
    //   * 控件族对引擎透明(类压根没注册), 它们的形参 `JZWidget` 永远解析不到类型 —— 类型由 TS 侧管;
    //   * 形如 `w.setProperty(name, JSValue)` 这种也一样(引擎没有对应类型号)。
    // ⚠ 这里必须显式短路: 旧代码会落到 isVectorType(JSType_none) 去查 m_typeMap(键是类型号, 没有
    //   -1) -> 读 end() = UB, 于是"同一个控件的形参校验时好时坏"(2026-09-18 那个偶发崩的根因)。
    if (type == JSType_none)
        return true;

    if (isVectorType(type))
    {
        if (!JS_IsArray(value))
            return false;

        int sub_type = m_typeMap[type].valueType;
        int64_t length = 0;
        JS_GetLength(m_ctx, value, &length);
        for (int i = 0; i < length; i++)
        {
            JSValue sub = JS_GetPropertyInt64(m_ctx, value, i);
            bool ret = checkType(sub, sub_type);
            JS_FreeValue(m_ctx, sub);
            if (!ret)
                return false;
        }

        return true;
    }
    int js_type = jsValueType(value);
    if (js_type >= JSType_object && type == JSType_jsonValue)
        return true;

    return canConvert(js_type, type);
}

bool JZJSEngine::checkType(JSValue value, std::string type_name)
{
    int type_id = typeNameToType(type_name);
    return checkType(value, type_id);
}

// =====================================================================
// 构造参数的"检测 + 取值"一步到位(checkAndSetArgs)
// =====================================================================
namespace {

// 单个形参描述(format 解析结果)
struct JZArgSpec
{
    enum Kind { kInt, kDouble, kBool, kString, kJson, kArray, kClass };
    Kind kind;
    bool hasDefault = false;
    int defInt = 0;
    double defDouble = 0;
    bool defBool = false;
    std::string defString;
    Json::Value defJson;
    std::string arrayElem;   // kArray: 元素类型(int/double/string/json/<类名>)
    int arrayLen = -1;       // kArray: 期望长度(-1 = 不限)
    std::string className;   // kClass: 注册类名
};

// 解析一个重载组(以 ',' 分隔的形参列表)
std::vector<JZArgSpec> jzParseArgGroup(const std::string& group)
{
    std::vector<JZArgSpec> out;
    size_t start = 0;
    while (start <= group.size())
    {
        size_t comma = group.find(',', start);
        std::string seg = (comma == std::string::npos) ? group.substr(start) : group.substr(start, comma - start);
        if (!seg.empty())
        {
            size_t eq = seg.find('=');
            std::string type = seg, def;
            if (eq != std::string::npos)
            {
                type = seg.substr(0, eq);
                def = seg.substr(eq + 1);
            }

            JZArgSpec s;
            if (type.rfind("array<", 0) == 0)
            {
                s.kind = JZArgSpec::kArray;
                size_t l = type.find('<');
                size_t r = type.find('>');
                s.arrayElem = type.substr(l + 1, r - l - 1);
                std::string rest = type.substr(r + 1);
                if (!rest.empty() && rest[0] == '[')
                {
                    size_t re = rest.find(']');
                    if (re != std::string::npos)
                        s.arrayLen = std::atoi(rest.substr(1, re - 1).c_str());
                }
            }
            else if (type == "int") s.kind = JZArgSpec::kInt;
            else if (type == "double" || type == "number") s.kind = JZArgSpec::kDouble;
            else if (type == "bool") s.kind = JZArgSpec::kBool;
            else if (type == "string") s.kind = JZArgSpec::kString;
            else if (type == "json" || type == "Json" || type == "Json::Value") s.kind = JZArgSpec::kJson;
            else { s.kind = JZArgSpec::kClass; s.className = type; }

            if (eq != std::string::npos)
            {
                s.hasDefault = true;
                if (s.kind == JZArgSpec::kInt) s.defInt = std::atoi(def.c_str());
                else if (s.kind == JZArgSpec::kDouble) s.defDouble = std::atof(def.c_str());
                else if (s.kind == JZArgSpec::kBool) s.defBool = (def == "true" || def == "1");
                else if (s.kind == JZArgSpec::kString)
                {
                    if (def.size() >= 2 && def.front() == '"' && def.back() == '"')
                        def = def.substr(1, def.size() - 2);
                    s.defString = def;
                }
            }
            out.push_back(s);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// 把单个 JS 数组值转成对应类型的 vector, 写入 out
bool jzConvertArray(JZJSEngine* engine, JSValueConst v, const JZArgSpec& spec, ArgResult& out)
{
    JSContext* ctx = engine->context();
    if (!JS_IsArray(v))
        return false;
    int64_t len = 0;
    JS_GetLength(ctx, v, &len);
    if (spec.arrayLen >= 0 && (int64_t)spec.arrayLen != len)
        return false;

    const std::string& elem = spec.arrayElem;
    if (elem == "int")
    {
        auto* vec = new std::vector<int>;
        vec->reserve((size_t)len);
        for (int64_t i = 0; i < len; i++)
        {
            JSValue e = JS_GetPropertyInt64(ctx, v, i);
            if (!engine->checkType(e, JSType_int))
            {
                JS_FreeValue(ctx, e);
                delete vec;
                return false;
            }
            int32_t x = 0;
            JS_ToInt32(ctx, &x, e);
            vec->push_back(x);
            JS_FreeValue(ctx, e);
        }
        out.vec_int_ptr = vec;
        out.kind = ArgResult::kVecInt;
        return true;
    }
    else if (elem == "double")
    {
        auto* vec = new std::vector<double>;
        vec->reserve((size_t)len);
        for (int64_t i = 0; i < len; i++)
        {
            JSValue e = JS_GetPropertyInt64(ctx, v, i);
            if (!JS_IsNumber(e))
            {
                JS_FreeValue(ctx, e);
                delete vec;
                return false;
            }
            double d = 0;
            JS_ToFloat64(ctx, &d, e);
            vec->push_back(d);
            JS_FreeValue(ctx, e);
        }
        out.vec_double_ptr = vec;
        out.kind = ArgResult::kVecDouble;
        return true;
    }
    else if (elem == "string")
    {
        auto* vec = new std::vector<std::string>;
        vec->reserve((size_t)len);
        for (int64_t i = 0; i < len; i++)
        {
            JSValue e = JS_GetPropertyInt64(ctx, v, i);
            if (!engine->checkType(e, JSType_string))
            {
                JS_FreeValue(ctx, e);
                delete vec;
                return false;
            }
            vec->push_back(::fromJSValue<std::string>(ctx, e));
            JS_FreeValue(ctx, e);
        }
        out.vec_string_ptr = vec;
        out.kind = ArgResult::kVecString;
        return true;
    }
    else if (elem == "json")
    {
        auto* vec = new std::vector<Json::Value>;
        vec->reserve((size_t)len);
        for (int64_t i = 0; i < len; i++)
        {
            JSValue e = JS_GetPropertyInt64(ctx, v, i);
            vec->push_back(::fromJSValue<Json::Value>(ctx, e));
            JS_FreeValue(ctx, e);
        }
        out.vec_json_ptr = vec;
        out.kind = ArgResult::kVecJson;
        return true;
    }
    else
    {
        // 类对象数组: 元素存裸指针
        auto* vec = new std::vector<void*>;
        vec->reserve((size_t)len);
        for (int64_t i = 0; i < len; i++)
        {
            JSValue e = JS_GetPropertyInt64(ctx, v, i);
            if (!engine->checkType(e, elem))
            {
                JS_FreeValue(ctx, e);
                delete vec;
                return false;
            }
            JSClassID cid = 0;
            void* raw = JS_GetAnyOpaque(e, &cid);
            if (!raw)
            {
                JS_FreeValue(ctx, e);
                delete vec;
                return false;
            }
            // 同 kClass: opaque 可能是守卫, 统一解成底层对象
            raw = engine->objectFromOpaque((int)cid, raw);
            if (!raw)
            {
                JS_FreeValue(ctx, e);
                delete vec;
                return false;
            }
            vec->push_back(raw);
            JS_FreeValue(ctx, e);
        }
        out.vec_ptr_ptr = vec;
        out.kind = ArgResult::kVecPtr;
        return true;
    }
}

// 把单个 JS 实参转换成 spec 描述的类型, 写入 out
bool jzConvertArg(JZJSEngine* engine, JSValueConst v, const JZArgSpec& spec, ArgResult& out)
{
    JSContext* ctx = engine->context();
    switch (spec.kind)
    {
    case JZArgSpec::kInt:
    {
        if (!engine->checkType(v, JSType_int)) return false;
        int32_t x = 0;
        JS_ToInt32(ctx, &x, v);
        out.i = x;
        out.kind = ArgResult::kInt;
        return true;
    }
    case JZArgSpec::kDouble:
    {
        if (!JS_IsNumber(v)) return false;
        double d = 0;
        JS_ToFloat64(ctx, &d, v);
        out.d = d;
        out.kind = ArgResult::kDouble;
        return true;
    }
    case JZArgSpec::kBool:
    {
        if (!JS_IsBool(v)) return false;
        out.b = (JS_ToBool(ctx, v) != 0);
        out.kind = ArgResult::kBool;
        return true;
    }
    case JZArgSpec::kString:
    {
        if (!engine->checkType(v, JSType_string)) return false;
        out.str_ptr = new std::string(::fromJSValue<std::string>(ctx, v));
        out.kind = ArgResult::kString;
        return true;
    }
    case JZArgSpec::kJson:
    {
        out.json_ptr = new Json::Value(::fromJSValue<Json::Value>(ctx, v));
        out.kind = ArgResult::kJson;
        return true;
    }
    case JZArgSpec::kArray:
        return jzConvertArray(engine, v, spec, out);
    case JZArgSpec::kClass:
    {
        bool typeOk = engine->checkType(v, spec.className);
        JSClassID cid = 0;
        void* ptr = JS_GetAnyOpaque(v, &cid);
        if (!typeOk) return false;
        if (!ptr) return false;
        // ⚠ opaque 未必就是底层对象: 指针语义的类(JZObject 派生 / 控件族那套 op 槽)存的是**守卫**。
        //   统一交给 objectFromOpaque 解 —— 它对两种都认(见 isGuardedClass), 直接把 opaque 当对象
        //   交出去会让 C++ 构造器拿到"守卫的地址"(形如 `new JZLabel(text, 守卫地址)` → 崩)。
        out.p = engine->objectFromOpaque((int)cid, ptr);
        if (!out.p) return false;
        out.kind = ArgResult::kPointer;
        return true;
    }
    }
    return false;
}

// 设置缺省值(省略的可选参数)
void jzSetDefault(const JZArgSpec& spec, ArgResult& out)
{
    switch (spec.kind)
    {
    case JZArgSpec::kInt:    out.i = spec.defInt; out.kind = ArgResult::kInt; break;
    case JZArgSpec::kDouble: out.d = spec.defDouble; out.kind = ArgResult::kDouble; break;
    case JZArgSpec::kBool:   out.b = spec.defBool; out.kind = ArgResult::kBool; break;
    case JZArgSpec::kString: out.str_ptr = new std::string(spec.defString); out.kind = ArgResult::kString; break;
    case JZArgSpec::kJson:   out.json_ptr = new Json::Value(spec.defJson); out.kind = ArgResult::kJson; break;
    case JZArgSpec::kClass:  out.p = nullptr; out.kind = ArgResult::kPointer; break;
    case JZArgSpec::kArray:
        if (spec.arrayElem == "int") { out.vec_int_ptr = new std::vector<int>; out.kind = ArgResult::kVecInt; }
        else if (spec.arrayElem == "double") { out.vec_double_ptr = new std::vector<double>; out.kind = ArgResult::kVecDouble; }
        else if (spec.arrayElem == "string") { out.vec_string_ptr = new std::vector<std::string>; out.kind = ArgResult::kVecString; }
        else if (spec.arrayElem == "json") { out.vec_json_ptr = new std::vector<Json::Value>; out.kind = ArgResult::kVecJson; }
        else { out.vec_ptr_ptr = new std::vector<void*>; out.kind = ArgResult::kVecPtr; }
        break;
    }
}

}  // namespace

int JZJSEngine::checkAndSetArgs(const std::string& format, std::vector<ArgResult>* result)
{
    // 按 '|' 拆分重载组
    std::vector<std::vector<JZArgSpec>> groups;
    {
        size_t start = 0;
        while (start <= format.size())
        {
            size_t bar = format.find('|', start);
            std::string g = (bar == std::string::npos) ? format.substr(start) : format.substr(start, bar - start);
            if (!g.empty())
            {
                if (g == "null" || g == "nullptr" || g == "undefined")
                    groups.push_back({});   // 零参数组: 匹配 argc == 0, 作为无参重载的"补位"
                else
                    groups.push_back(jzParseArgGroup(g));
            }
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
    }

    int argc = m_currentArgc;
    JSValueConst* argv = m_currentArgv;
    for (size_t gi = 0; gi < groups.size(); gi++)
    {
        const std::vector<JZArgSpec>& args = groups[gi];
        int total = (int)args.size();
        int required = total;
        for (const auto& a : args)
            if (a.hasDefault) required--;
        if (argc < required || argc > total)
            continue;

        std::vector<ArgResult> out(total);
        bool ok = true;
        for (int i = 0; i < total && ok; i++)
        {
            if (i < argc)
            {
                if (!jzConvertArg(this, argv[i], args[i], out[i]))
                    ok = false;
            }
            else
            {
                jzSetDefault(args[i], out[i]);
            }
        }
        if (ok)
        {
            *result = std::move(out);
            return (int)gi;
        }
    }

    JS_ThrowTypeError(m_ctx, "参数不匹配: 期望 %s", format.c_str());
    return -1;
}

JSValue JZJSEngine::makeParamNumError(int num)
{
    return JS_ThrowTypeError(m_ctx, "需要%d个参数", num);
}

string JZJSEngine::makeTypeErrorText(int i, int type)
{
    return str_asprintf("输入%d需要类型%s", i, typeName(type).c_str());
}

string JZJSEngine::makeGetPropErrorText(std::string prop)
{
    return str_asprintf("读取属性%s失败", prop.c_str());
}

JSValue JZJSEngine::makeTypeError(int i, int type)
{
    return JS_ThrowTypeError(m_ctx, "输入%d需要类型%s", i, typeName(type).c_str());
}

JSValue JZJSEngine::makeOpenFileError(string file)
{
    return JS_ThrowPlainError(m_ctx, "打开文件\"%s\"失败", file.c_str());
}

JSValue JZJSEngine::globalObject()
{
    return m_global;
}

JSValue JZJSEngine::createObject()
{
    JSValue obj = JS_NewObject(m_ctx);
    return obj;
}

JSValue JZJSEngine::createValue(int type, int argc, JSValueConst* argv)
{
    m_currentArgc = argc;
    m_currentArgv = argv;

    if (type == JSType_bool)
    {
        return JS_NewBool(m_ctx, 0);
    }
    else if (type == JSType_int)
    {
        return JS_NewInt32(m_ctx, 0);
    }
    else if (type == JSType_double)
    {
        return JS_NewNumber(m_ctx, 0.0);
    }
    else if (type == JSType_string)
    {
        return JS_NewString(m_ctx, "");
    }
    else
    {
        const CClassDefine& define = m_typeMap[type].classDefine;
        void* ptr = define.create(this, argc, argv);
        
        // create 里参数不合法时不再抛 C++ 异常, 而是"JS_Throw* + 返回 nullptr" ——
        // 这里检测到空指针就说明已经挂了 JS 异常, 直接把它交给 JS(不建对象)。
        // 没挂异常却返回空 = create 写错了(断言会 fatal 掉, 免得吞成一个空异常)。
        if (!ptr)
        {
            JZ_ASSERT_X(JS_HasException(m_ctx), "createValue: create 返回空指针但没有设置 JS 异常");
            return JS_EXCEPTION;
        }

        // 通知绑定层那一步在 createValue(type, ptr) 里(所有创建路径共用一处)
        JSValue obj = createValue(type, ptr);
        return obj;
    }
}

JSValue JZJSEngine::createValue(int type, void *ptr)
{
    JSClassID js_class_id = typeToJSType(type);
    JSValue proto = JS_GetClassProto(m_ctx, js_class_id);

    JSValue obj = JS_NewObjectProtoClass(m_ctx, proto, js_class_id);

    // 指针语义类(JZObject 派生): opaque 存**守卫**(JZObjectPointer 的堆副本)而不是裸指针 ——
    // 所有权在 JS(JS 对象终结时 delete), 但对象也可能先被 C++ 删掉, 守卫负责认出"已经没了"。
    // 其余类(值类型/JZPainter/item)保持裸指针, JS 拥有。
    if (isGuardedClass((int)js_class_id))
        JS_SetOpaque(obj, new JZObjectPointer(((JZObject*)ptr)->guardPointer()));
    else
        JS_SetOpaque(obj, ptr);

    JS_FreeValue(m_ctx, proto);

    // 对象造好了 -> 通知绑定层(把宿主句柄记到 C++ 对象上 + 连信号, 见 setObjectCreatedCallback)。
    // ⚠ 这条通知必须**所有**创建路径都发: 不只是 `new X()`(下面 createValue(type,argc,argv)),
    //   还有 toJsValueFromPtr(把 C++ 侧持有的对象交给 JS, 如菜单/菜单项句柄) —— 绑定层正是靠它
    //   把 onXxx 信号接到这份 JS 包装上; 漏发的话句柄上的 onTriggered 之类永远不响。
    // 传借用值: 回调要留就自己 dup。
    // 注意: ptr 未必是 JZObject(值类型 JZRect/JZColor/JZImage... 就不是) —— 绑定层自己用
    // isGuardedType(type) 分辨, 别直接当 JZObject 用。
    if (m_objectCreated && !JS_IsException(obj))
        m_objectCreated(type, obj, ptr);

    return obj;
}

void JZJSEngine::disarmOpaque(JSValue obj)
{
    // "视图对象"用完作废: 释放守卫并置空 opaque, 之后任何终结/调用都从空 opaque 出发。
    // 用途: 借给 JS 的对象是 C++ 栈对象(事件)、或 C++ 自己负责生命周期的对象 ——
    // 不能让 JS 对象将来按守卫去 delete 它。
    if (!JS_IsObject(obj))
        return;

    JSClassID class_id = 0;
    void* opaque = JS_GetAnyOpaque(obj, &class_id);
    if (!opaque)
        return;

    if (isGuardedClass((int)class_id))
        delete (JZObjectPointer*)opaque;

    JS_SetOpaque(obj, nullptr);
}

bool JZJSEngine::isGuardedClass(int class_id) const
{
    // 透明控件类的 op class 也走守卫(opaque = JZObjectPointer*): 它的类没进 m_typeMap(控件族对
    // 引擎透明), 所以由 widget 模块把 class id 告诉引擎 —— 不然 fromJSValue<JZWidget*> 会把守卫
    // 当裸指针交出去。
    if (m_widgetOpClassId != 0 && class_id == m_widgetOpClassId)
        return true;

    auto jt = m_jsTypeMap.find(class_id);
    if (jt == m_jsTypeMap.end())
        return false;

    auto tt = m_typeMap.find(jt->second);
    if (tt == m_typeMap.end())
        return false;

    return tt->second.classDefine.guardedOpaque;
}

void* JZJSEngine::objectFromOpaque(int class_id, void* opaque)
{
    if (!opaque)
        return nullptr;

    if (!isGuardedClass(class_id))
        return opaque;

    JZObject* object = ((JZObjectPointer*)opaque)->object();
    if (!object)
    {
        // 上层通常已用 checkFunctionArg/js_check_class_arg 拦过, 走到这里说明对象在调用期间被销毁
        // (父控件被删 / 派发中途 deleteLater 生效)。给 JS 一个明确异常, 而不是让它读悬垂内存。
        JS_ThrowTypeError(m_ctx, "对象已被销毁(底层 C++ 对象不存在了)");
        return nullptr;
    }
    return object;
}

int JZJSEngine::retainCallback(JSValueConst cb)
{
    if (!m_ctx || JS_IsUndefined(m_cbRoot))
        return -1;

    int id = m_cbId++;
    // 根数组持有引用, 使闭包对 GC 始终可达。
    // 注意: JS_SetPropertyInt64 会消费传入值的引用, cb 是借来的参数, 必须先 dup 一份。
    JS_SetPropertyInt64(m_ctx, m_cbRoot, id, JS_DupValue(m_ctx, cb));
    return id;
}

void JZJSEngine::releaseCallback(int id, JSValue value)
{
    if (!m_ctx)
        return;

    if (id >= 0 && !JS_IsUndefined(m_cbRoot))
        JS_SetPropertyInt64(m_ctx, m_cbRoot, id, JS_UNDEFINED);   // 释放根数组对该回调的引用
    JS_FreeValue(m_ctx, value);
}

void JZJSEngine::freeValue(JSValue value)
{
    JS_FreeValue(m_ctx, value);
}

JSValue JZJSEngine::dupValue(JSValue value)
{
    return JS_DupValue(m_ctx, value);
}

JSValue JZJSEngine::createResolvedPromise(JSValue jsValue)
{
    // 1. 创建 Promise + resolve/reject 函数
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(m_ctx, resolving_funcs);

    JS_FreeValue(m_ctx, callJs(resolving_funcs[0], JS_UNDEFINED, 1, &jsValue));

    // 4. 释放临时值
    JS_FreeValue(m_ctx, resolving_funcs[0]);
    JS_FreeValue(m_ctx, resolving_funcs[1]);

    // 5. 返回已经 resolve 完成的 Promise
    return promise;
}

JSValue JZJSEngine::createRejectPromise(JSValue jsValue)
{
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(m_ctx, resolving_funcs);

    JS_FreeValue(m_ctx, callJs(resolving_funcs[1], JS_UNDEFINED, 1, &jsValue));

    JS_FreeValue(m_ctx, resolving_funcs[0]);
    JS_FreeValue(m_ctx, resolving_funcs[1]);

    return promise;
}

void JZJSEngine::setProperty(JSValue obj, std::string name, JSValue value)
{
    JS_SetPropertyStr(m_ctx, obj, name.c_str(), value);
}

JSValue JZJSEngine::getProperty(JSValue obj, std::string name)
{
    return JS_GetPropertyStr(m_ctx, obj, name.c_str());
}

JZJSValuePtr JZJSEngine::makeValuePtr(JSValue value)
{
    return JZJSValuePtr(m_ctx, value);
}

void JZJSEngine::setPromiseResolve(int handle, const std::vector<JZJSValuePtr>& result)
{
    PromiseTask* task = getPromiseTask(handle);
    if (!task)
        return;

    std::vector<JSValue> value_list;
    for (int i = 0; i < result.size(); i++)
    {
        value_list.push_back(result[i].get());
    }

    // promise 已结算, then/await 链的微任务入队 -> callJs 内部的检查点会唤醒引擎泵一轮
    JSValue ret = callJs(task->resolve, JS_UNDEFINED, (int)value_list.size(), value_list.data());
    JS_FreeValue(m_ctx, ret);   // JS_Call 返回的引用需要释放, 否则泄漏
    map_remove(m_promiseTask, handle);
}

void JZJSEngine::setPromiseReject(int handle, const std::string& error_message)
{
    PromiseTask* task = getPromiseTask(handle);
    if (!task)
        return;

    JSValue error = JS_NewError(m_ctx);
    JSValue msg = JS_NewString(m_ctx, error_message.c_str());
    JS_SetPropertyStr(m_ctx, error, "message", msg);

    JSValue args[] = {error};
    // reject 触发未处理拒绝 / await 继续等微任务 -> callJs 内部的检查点负责唤醒
    JSValue ret = callJs(task->reject, JS_UNDEFINED, 1, args);
    fprintf(stderr, "[promise] reject handle=%d isExc=%d\n", handle, (int)JS_IsException(ret));
    fflush(stderr);
    JS_FreeValue(m_ctx, ret);   // JS_Call 返回的引用需要释放, 否则泄漏
    JS_FreeValue(m_ctx, error);

    map_remove(m_promiseTask, handle);
}

void JZJSEngine::deconstructor(JSValue value)
{
    int type = jsValueType(value);
    assert(type != JSType_none);

    JSClassID class_id = JS_GetClassID(value);

    auto ptr = JS_GetOpaque(value, class_id);
    m_typeMap[type].classDefine.destory(ptr);
}

// 单个形参的转换前校验(同步 / 异步共用): 类型 + JSRawPointer 精确类 + 是否已 clear。
// 校验通过后 fromJSValue 内部的类型/失效分支就只剩 JZ_ASSERT 兜底(属于上层漏校验)。
bool JZJSEngine::checkFunctionArg(const CFunctionParam& param, int index, JSValueConst value)
{
    int arg_type = param.type;
    if (arg_type == JSType_none)  // 前向引用: 调用时再按 ctypeid 解析(此时类已注册)
        arg_type = ctypeidToType(param.ctypeid);
    // JSValue(原始 JS 值)参数不做类型检查: 任意 JS 值都可以传
    if (arg_type == JSType_oriValue)
        return true;

    // 对象指针形参(JZObjectPointer*): 控件族对引擎透明 —— 它们在类型表里没有各自的类型号
    // (所有控件共用一个 op class), 所以这里**不走类型号比较**, 直接按"opaque 是不是活着的
    // 对象指针"判。形参写 JZObjectPointer* 的意义就在这一层: 传数字/字符串/普通对象会被拦下,
    // 而不是拖到方法体里再当指针用。
    if (arg_type == JSType_objectPointer)
    {
        if (JS_IsNull(value) || JS_IsUndefined(value))
        {
            if (param.allowNull)
                return true;             // 允许空(如 parent/owner)由注册处声明 allowNull
            // 返回 false 必须**带着 pending 异常**(调用方有 JZ_ASSERT_X(JS_HasException) 兜底)
            JS_ThrowTypeError(m_ctx, "参数错误:参数%d需要控件对象(不能为空)", index);
            return false;
        }
        JSClassID class_id = 0;
        void* opaque = JS_GetAnyOpaque(value, &class_id);
        if (!opaque || !isGuardedClass((int)class_id))
        {
            JS_ThrowTypeError(m_ctx, "参数错误:参数%d需要控件对象", index);
            return false;
        }
        // 守卫失效(底层对象已被删)时 objectFromOpaque 自己抛异常
        return objectFromOpaque((int)class_id, opaque) != nullptr;
    }

    int js_type = jsValueType(value);
    if (js_type == JSType_null && arg_type > JSType_object && param.allowNull)
        return true;

    // 指针语义的类参数必须能解析出**还活着**的 C++ 对象, 两种情况都在这里拦成 JS 异常:
    //   * opaque 为空: 被 disarm 过的视图(绘制派发结束后的 JZPainter、事件视图) —— 类还是那个类,
    //     但已经不对应任何 C++ 对象;
    //   * opaque 是守卫但守卫已失效: 底层对象被 C++ 删掉了(如布局被 clearLayout/控件析构收走)。
    // 必须拦在这一步: 否则 fromJSValue 会给出**空指针**, 直接进 C++ 方法就是解空指针(崩), 而不是
    // 可捕获的 JS 异常。objectFromOpaque 对"守卫失效"会自己抛异常, 这里只管 return false。
    if (arg_type > JSType_object && js_type != JSType_null && js_type != JSType_undefined)
    {
        JSClassID class_id = 0;
        void* opaque = JS_GetAnyOpaque(value, &class_id);
        if (!opaque)
        {
            JS_ThrowTypeError(m_ctx, "参数%d对应的对象已失效", index);
            return false;
        }
        if (!objectFromOpaque((int)class_id, opaque))
            return false;
    }

    if (js_type != arg_type && !canConvert(js_type, arg_type))
    {
        string input_type = typeName(js_type);
        string need_type = typeName(arg_type);
        std::string error = "参数错误:参数" + to_string(index) + "需要类型" + need_type + "，输入是" + input_type;
        JS_ThrowTypeError(m_ctx, "%s", error.c_str());
        return false;
    }
    
    return true;
}

JSValue JZJSEngine::call(int magic, int argc, JSValueConst* argv)
{
    CFunction* func = m_funcList[magic].get();
    if (argc != func->params.size())
    {
        std::string error = "参数错误:需要" + to_string(func->params.size()) + "个参数";
        return JS_ThrowTypeError(m_ctx, "%s", error.c_str());
    }

    for (int i = 0; i < (int)func->params.size(); i++)
    {
        if (!checkFunctionArg(func->params[i], i, argv[i]))
        {
            // 返回 JS_EXCEPTION 前必须有 pending 异常, 否则上层会拿到一个"取不到内容的空异常"
            JZ_ASSERT_X(JS_HasException(m_ctx), "checkFunctionArg 失败但未设置 JS 异常");
            return JS_EXCEPTION;
        }
    }

    try
    {
        return func->call(m_ctx, argv);
    }
    catch (std::exception& e)
    {
        return JS_ThrowInternalError(m_ctx, "%s", e.what());
    }
}

JSValue JZJSEngine::callAsync(int magic, int argc, JSValueConst* argv)
{
    CFunction* func = m_funcList[magic].get();
    if (argc != func->params.size() - 1)
    {
        std::string error = "参数错误:需要" + to_string(func->params.size() - 1) + "个参数";
        return JS_ThrowTypeError(m_ctx, "%s", error.c_str());
    }

    // 与同步 call 用同一套校验(含 JSRawPointer 形参的精确类检查) —— 之前这里漏了精确类那条,
    // 会让派生类对象落到 fromJSValue 的断言上。
    for (int i = 0; i < argc; i++)
    {
        if (!checkFunctionArg(func->params[i + 1], i, argv[i]))
        {
            JZ_ASSERT_X(JS_HasException(m_ctx), "checkFunctionArg 失败但未设置 JS 异常");
            return JS_EXCEPTION;
        }
    }

    int promise_handle = m_promiseTaskId++;

    std::shared_ptr<PromiseTask> task = std::make_shared<PromiseTask>();
    task->ctx = m_ctx;

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(m_ctx, resolving_funcs);
    task->resolve = JS_DupValue(m_ctx, resolving_funcs[0]);
    task->reject = JS_DupValue(m_ctx, resolving_funcs[1]);
    // JS_NewPromiseCapability 返回的 resolving_funcs 带 +1 引用, 调用方必须释放,
    // 否则这两个闭包对象会泄漏, 导致 JS_FreeRuntime 时 gc_obj_list 非空而断言失败
    JS_FreeValue(m_ctx, resolving_funcs[0]);
    JS_FreeValue(m_ctx, resolving_funcs[1]);
    m_promiseTask[promise_handle] = task;

    JSValue handle_value = JS_NewInt32(m_ctx, promise_handle);
    try
    {
        vector<JSValue> async_args;
        async_args.push_back(handle_value);
        for (int i = 0; i < argc; i++)
        {
            async_args.push_back(argv[i]);
        }
        func->call(m_ctx, async_args.data());

        // 注意: promise 的所有权转移给调用方(JS 异步函数返回值), 这里不能释放,
        // 否则返回的 promise 已被释放, 后续 GC 对该对象再 decref 会触发 ref_count 断言
        JS_FreeValue(m_ctx, handle_value);
        return promise;
    }
    catch (std::exception& e)
    {
        map_remove(m_promiseTask, promise_handle);
        JS_FreeValue(m_ctx, promise);
        JS_FreeValue(m_ctx, handle_value);

        // fromJSValue 触发已 clear 的 JSRawPointer 时已先 JS_ThrowTypeError, 优先复用该异常
        return JS_ThrowInternalError(m_ctx, "%s", e.what());
    }
}

std::string JZJSEngine::runtimeError()
{
    return m_runtimeError;
}

void JZJSEngine::onExceptionOccurred(std::string error)
{
    m_runtimeError = error;

    setStatus(Engine_error);
    stopEngineTimer();

    sigLog.emit(m_runtimeError);
    sigError.emit(m_runtimeError);
}

JSContext* JZJSEngine::context()
{
    return m_ctx;
}

JZJSValuePtr JZJSEngine::eval(const std::string& code)
{
    JSValue result = JS_Eval(m_ctx, code.c_str(), code.size(), "eval", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result))
        unHandleException();
    else
        wakeUp();   // eval 内可能产生 promise 微任务或注册 JS timer, 下一轮事件循环泵/调度

    return JZJSValuePtr(m_ctx, result);
}

std::vector<JZJSValuePtr> JZJSEngine::callFunction(const std::string& func, const std::vector<JZJSValuePtr>& params)
{
    std::vector<JZJSValuePtr> result_list;
    if (!m_ctx)
        return result_list;

    JSValue fn = JS_GetPropertyStr(m_ctx, m_global, func.c_str());
    if (JS_IsException(fn))
    {
        JS_FreeValue(m_ctx, fn);
        unHandleException();
        return result_list;
    }
    if (!JS_IsFunction(m_ctx, fn))
    {
        JS_FreeValue(m_ctx, fn);
        JS_ThrowReferenceError(m_ctx, "'%s' is not a function", func.c_str());
        unHandleException();
        return result_list;
    }

    vector<JSValue> args;
    args.reserve(params.size());
    for (size_t i = 0; i < params.size(); i++)
        args.push_back(params[i].get());

    // 被调函数可能注册 timer / 产生微任务: callJs 内部会补一次微任务检查点
    JSValue ret = callJs(fn, JS_UNDEFINED, (int)args.size(), args.empty() ? nullptr : args.data());
    JS_FreeValue(m_ctx, fn);
    if (JS_IsException(ret))
    {
        unHandleException();
        return result_list;
    }

    // 返回值所有权直接交给 JZJSValuePtr(与 eval 一致)
    result_list.push_back(JZJSValuePtr(m_ctx, ret));
    return result_list;
}

std::vector<JZJSValuePtr> JZJSEngine::callMemberFunction(const std::string& func, const JZJSValuePtr &this_val, const std::vector<JZJSValuePtr>& params)
{
    std::vector<JZJSValuePtr> result_list;
    if (!m_ctx)
        return result_list;

    JSValue obj = this_val.get();
    JSValue fn = JS_GetPropertyStr(m_ctx, obj, func.c_str());
    if (JS_IsException(fn))
    {
        JS_FreeValue(m_ctx, fn);
        unHandleException();
        return result_list;
    }
    if (!JS_IsFunction(m_ctx, fn))
    {
        JS_FreeValue(m_ctx, fn);
        JS_ThrowReferenceError(m_ctx, "'%s' is not a function", func.c_str());
        unHandleException();
        return result_list;
    }

    vector<JSValue> args;
    args.reserve(params.size());
    for (size_t i = 0; i < params.size(); i++)
        args.push_back(params[i].get());

    // 引擎 -> JS 这个方向也压一份 this: 回调(如 onPaint / onClicked)是箭头函数时拿不到 this,
    // 在回调里 self() 取的就是这里压的 obj(见 JZJSEngine::self)。
    JSValue ret;
    {
        SelfScope self_scope(this, obj);
        ret = callJs(fn, obj, (int)args.size(), args.empty() ? nullptr : args.data());
    }
    JS_FreeValue(m_ctx, fn);
    if (JS_IsException(ret))
    {
        unHandleException();
        return result_list;
    }

    // 返回值所有权直接交给 JZJSValuePtr(与 eval 一致)
    result_list.push_back(JZJSValuePtr(m_ctx, ret));
    return result_list;
}

int JZJSEngine::callAsyncFunction(const std::string& func, const std::vector<JZJSValuePtr>& params)
{
    if (!m_ctx)
        return -1;

    JSValue fn = JS_GetPropertyStr(m_ctx, m_global, func.c_str());
    if (JS_IsException(fn))
    {
        JS_FreeValue(m_ctx, fn);
        unHandleException();
        return -1;
    }
    if (!JS_IsFunction(m_ctx, fn))
    {
        JS_FreeValue(m_ctx, fn);
        JS_ThrowReferenceError(m_ctx, "'%s' is not a function", func.c_str());
        unHandleException();
        return -1;
    }

    vector<JSValue> args;
    args.reserve(params.size());
    for (size_t i = 0; i < params.size(); i++)
        args.push_back(params[i].get());

    JSValue ret = callJs(fn, JS_UNDEFINED, (int)args.size(), args.empty() ? nullptr : args.data());
    JS_FreeValue(m_ctx, fn);
    if (JS_IsException(ret))
    {
        unHandleException();
        return -1;
    }

    int handle = m_promiseTaskId++;

    // PromiseLike(对象且有函数 then) => 挂 resolve/reject 回调, 等引擎 job 泵结算
    JSValue then_fn = JS_GetPropertyStr(m_ctx, ret, "then");
    if (JS_IsException(then_fn))
    {
        JS_FreeValue(m_ctx, then_fn);
        JS_FreeValue(m_ctx, ret);
        unHandleException();
        return -1;
    }
    if (!JS_IsFunction(m_ctx, then_fn))
    {
        // 返回值不是 PromiseLike: 同步普通值, 立即结算
        JS_FreeValue(m_ctx, then_fn);
        JZAsyncResult ar;
        ar.result = handle;
        ar.params.push_back(JZJSValuePtr(m_ctx, ret));   // 转移所有权
        sigAsyncResult.emit(ar);
        return handle;
    }

    JSValue resolve_fn = JS_NewCFunctionMagic(m_ctx, JSInter::js_call_async_resolve, "resolve", 1,
                                              JS_CFUNC_generic_magic, handle);
    JSValue reject_fn = JS_NewCFunctionMagic(m_ctx, JSInter::js_call_async_reject, "reject", 1,
                                             JS_CFUNC_generic_magic, handle);

    JSValue then_args[2] = { resolve_fn, reject_fn };
    // then 回调入队也是微任务: 走 callJs 补检查点
    JSValue then_ret = callJs(then_fn, ret, 2, then_args);
    JS_FreeValue(m_ctx, then_fn);
    JS_FreeValue(m_ctx, resolve_fn);
    JS_FreeValue(m_ctx, reject_fn);
    JS_FreeValue(m_ctx, ret);
    if (JS_IsException(then_ret))
    {
        unHandleException();
        return -1;
    }
    JS_FreeValue(m_ctx, then_ret);
    return handle;
}

bool JZJSEngine::readModuleCode(const std::string& module_name, std::string& code,
                                std::string* resolved)
{
    // 候选路径: 原名 -> 补 .js -> 目录式 index.js
    std::vector<std::string> cands;
    cands.push_back(module_name);
    bool has_js_ext = (module_name.size() > 3 &&
                       module_name.compare(module_name.size() - 3, 3, ".js") == 0);
    if (!has_js_ext)
        cands.push_back(module_name + ".js");
    cands.push_back(module_name + "/index.js");

    for (size_t i = 0; i < cands.size(); i++)
    {
        std::string full = cands[i];
        bool is_abs = (!full.empty() && (full[0] == '/' || full[0] == '\\')) ||
                      (full.size() > 2 && full[1] == ':');

        // 解析基目录: 先 workDir, 再 library dirs; 绝对路径或都空时直接用原名
        std::vector<std::string> tryPaths;
        if (is_abs)
        {
            tryPaths.push_back(full);
        }
        else if (!m_workDir.empty() || !m_libraryDirs.empty())
        {
            if (!m_workDir.empty())
                tryPaths.push_back(JZDir(m_workDir).absoluteFilePath(full));
            for (const auto& d : m_libraryDirs)
                tryPaths.push_back(JZDir(d).absoluteFilePath(full));
        }
        else
        {
            tryPaths.push_back(full);   // 无基目录: 按相对路径直接尝试(原行为)
        }

        bool found = false;
        for (const auto& p : tryPaths)
        {
            JZFile file;
            if (!file.open(p.c_str(), "rb"))
                continue;
            JZByteArray bytes = file.readAll();
            file.close();

            code.assign(bytes.data(), (size_t)bytes.size());
            // 剥离 UTF-8 BOM
            if (code.size() >= 3 && (unsigned char)code[0] == 0xEF &&
                (unsigned char)code[1] == 0xBB && (unsigned char)code[2] == 0xBF)
                code.erase(0, 3);
            found = true;
            if (resolved)
                *resolved = p;   // 实际命中的路径(供 appFile)
            break;
        }
        if (found)
            return true;
    }
    return false;
}

bool JZJSEngine::builtinBridgeCode(const std::string& name, std::string& code)
{
    const BuiltinModule* m = findBuiltinModule(name);
    if (!m)
        return false;

    // 桥接源码里的 specifier 写"相对 libraryDirs 的裸名"(如 'http/dist/index.js'),
    // 与各包互相 import 的写法一致: 默认 normalizer 对不含 './' 的说明符
    // 原样交给 loader, 于是照旧按 readModuleCode 的 workDir -> libraryDirs 顺序命中。
    string file(m->file);

    // export * : 该包出口里的具名导出都可见 ⇒ `import { createServer } from 'http'` 成立
    //            (一名字一包之后, 模块成员就是包的具名导出, 不再有"藏在别人包里的成员").
    code = "export * from '" + file + "';\n";
    if (!m->exportName)
        code += "export { default } from '" + file + "';\n";       // 包自带 default(常规形态)
    else if (string(m->exportName) == "*")
        ;   // namespace 形态: export * 已足够, 没有 default
    else
        code += "export { " + string(m->exportName) + " as default } from '" + file + "';\n";
    return true;
}

JSModuleDef* JZJSEngine::loadModuleDef(const std::string& module_name)
{
    if (!m_ctx)
        return NULL;

    // Node 风格内置模块(http / fs / node:http ...): 它不是文件, 由 builtinBridgeCode
    // 合成一段桥接源码(把包 dist 出口里的那个导出提升为 default)。这一步同时让
    // `import http from 'http'` 可用 —— 在此之前写裸名只会得到 "could not load module"。
    std::string code;
    bool builtin = builtinBridgeCode(module_name, code);
    if (!builtin && !readModuleCode(module_name, code))
    {
        JS_ThrowReferenceError(m_ctx, "could not load module '%s'", module_name.c_str());
        return NULL;
    }

    JSValue module_val = JS_Eval(m_ctx, code.data(), code.size(), module_name.c_str(),
                                 JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(module_val))
    {
        // 编译失败: 保留内部错误文本, 再包一层便于定位是哪个模块文件
        JSValue exc = JS_GetException(m_ctx);
        std::string inner = getExceptionDescript(exc);
        JS_FreeValue(m_ctx, exc);
        JS_ThrowReferenceError(m_ctx, "could not compile module '%s': %s",
                               module_name.c_str(), inner.c_str());
        return NULL;
    }

    JSModuleDef* m = (JSModuleDef*)JS_VALUE_GET_PTR(module_val);
    JS_FreeValue(m_ctx, module_val);  // 模块定义由 ctx 的 loaded_modules 列表持有
    return m;
}

bool JZJSEngine::runModuleFile(const std::string& filepath)
{
    return runModuleFileImpl(filepath, nullptr);
}

bool JZJSEngine::tryRunModuleFile(const std::string& filepath, std::string* error)
{
    return runModuleFileImpl(filepath, error);
}

bool JZJSEngine::runModuleFileImpl(const std::string& filepath, std::string* error)
{
    if (!m_ctx)
    {
        if (error)
            *error = "engine 未初始化";
        return false;
    }

    // 取出挂起的异常文本(取走后 ctx 不再有 pending exception)。
    // error != nullptr 的分支全靠它把异常转成回填文本 —— 不能走 unHandleException,
    // 那条路会把引擎打成 Engine_error(宿主会当成 JS 崩了)。
    auto takePending = [this]() -> std::string {
        JSValue exc = JS_GetException(m_ctx);
        std::string text = getExceptionDescript(exc);
        JS_FreeValue(m_ctx, exc);
        return text;
    };

    // quickjs 默认模块 normalizer 按 '/' 切分基名, 统一转成 '/' 分隔
    std::string module_name = filepath;
    std::replace(module_name.begin(), module_name.end(), '\\', '/');

    std::string code;
    std::string resolved;
    if (!readModuleCode(module_name, code, &resolved))
    {
        std::string text = "could not load module '" + module_name + "'";
        if (error)
        {
            *error = text;
            return false;
        }
        JS_ThrowReferenceError(m_ctx, "%s", text.c_str());
        unHandleException();
        return false;
    }

    // 入口脚本 = **第一个成功加载的模块**(一般是 main.js)。引擎预加载内部库时那个也算数,
    // 所以宿主应用 setAppFile 显式指定真正的用户入口(见 JZEngineRuntime::runModule)。
    if (m_appFile.empty() && !resolved.empty())
        m_appFile = resolved;

    // COMPILE_ONLY: 只编译, 拿到模块定义(JS_TAG_MODULE -> JSModuleDef*)
    JSValue module_val = JS_Eval(m_ctx, code.data(), code.size(), module_name.c_str(),
                                 JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(module_val))
    {
        std::string text = "could not compile module '" + module_name + "': " + takePending();
        if (error)
        {
            *error = text;
            return false;
        }
        JS_ThrowReferenceError(m_ctx, "%s", text.c_str());
        return false;
    }

    // 实例化(经 loader 拉取 import 依赖)并求值。module_val 的引用在此被消费,
    // 模块定义继续由 ctx 的 loaded_modules 列表持有。
    JSValue result = JS_EvalFunction(m_ctx, module_val);
    if (JS_IsException(result))
    {
        std::string text = takePending();
        if (error)
        {
            *error = text.empty() ? ("evaluate module failed: " + module_name) : text;
            return false;
        }
        JS_ThrowInternalError(m_ctx, "%s", text.c_str());
        unHandleException();
        return false;
    }
    JS_FreeValue(m_ctx, result);

    // 模块顶层代码的错误会转为 promise 拒绝, 泵完 job 队列才反映到 runtimeError
    pumpPendingJobs();

    // 模块顶层可能注册了 JS 定时任务, 挂上一次性 timer(事件驱动; 无任务则保持空闲)
    if (m_status == Engine_running)
        scheduleNextTimer();

    return true;
}

void JZJSEngine::execOnce()
{
    if (!m_ctx || isInterruption())
        return;    

    // 执行 set timer
    while (true)
    {
        int64_t now = currentMs();
        int64_t max_gap = -1;   // 用 -1 作初值, 任意 cur_gap >= 0 都视为到期

        int max_task = -1;
        for (int i = 0; i < (int)m_timeTasks.size(); i++)
        {
            int64_t cur_gap = now - m_timeTasks[i].next_run_time;
            if (cur_gap >= 0 && cur_gap > max_gap)
            {
                max_gap = cur_gap;
                max_task = i;
            }
        }
        if (max_task == -1)
            break;

        // 记录 id: 回调内部可能调用 clearInterval 删除本任务, 之后不能再按旧索引访问
        int task_id = m_timeTasks[max_task].id;

        // 复制一份独立引用再执行: 回调内部可能调用 clearInterval 释放自身,
        // 若不持有独立引用, JS_Call 执行期间该 JS 对象会被提前释放导致崩溃
        JSValue callback = JS_DupValue(m_ctx, m_timeTasks[max_task].callback);
        // 这里不用 callJs: 本函数自己就是泵(尾部 pumpPendingJobs 会把本轮产生的 job 排空),
        // 再投一次唤醒只会白跑一轮。
        JSValue result = JS_Call(m_ctx, callback, JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(m_ctx, callback);

        if (JS_IsException(result))
        {
            unHandleException();
            JS_FreeValue(m_ctx, result);

            // 移除本次触发失败的定时器任务: 否则 resetUncaughtError 之后它会再次
            // 到期并重复触发(回调必然再次抛异常)。clearInterval 已在回调内执行的
            // 情况由 task_id 查找保护, 找不到则跳过。
            for (int i = 0; i < (int)m_timeTasks.size(); i++)
            {
                if (m_timeTasks[i].id == task_id)
                {
                    JS_FreeValue(m_ctx, m_timeTasks[i].callback);
                    vec_removeAt(m_timeTasks, i);
                    break;
                }
            }
            return;
        }

        JS_FreeValue(m_ctx, result);

        // 按 id 重新查找该任务, 若已被 clearInterval 删除则跳过重排
        int idx = -1;
        for (int i = 0; i < (int)m_timeTasks.size(); i++)
        {
            if (m_timeTasks[i].id == task_id)
            {
                idx = i;
                break;
            }
        }
        if (idx == -1)
            continue;

        if (m_timeTasks[idx].runOnce)
        {
            JS_FreeValue(m_ctx, m_timeTasks[idx].callback);
            vec_removeAt(m_timeTasks, idx);
        }
        else
        {
            m_timeTasks[idx].next_run_time += m_timeTasks[idx].interval;
        }
    }

    // 执行 promise
    pumpPendingJobs();

    // 跑完一轮后按最近到期任务续期一次性 timer(出错时引擎已停表进入 error, 不再续期,
    // 由 resetUncaughtError/resume 恢复)
    if (m_status == Engine_running)
        scheduleNextTimer();
}

void JZJSEngine::pumpPendingJobs()
{
    if (!m_ctx)
        return;

    JSContext* pend_ctx = nullptr;
    while (true)
    {
        int ret = JS_ExecutePendingJob(m_rt, &pend_ctx);
        if (ret == 0)
            break;
        else if (ret < 0)
        {
            unHandleException();
            return;
        }
        else if (ret == 1)
        {
            // ⚠ 这里**不能**顺手判"未处理拒绝": 一个 job 结束时刚被拒的 promise 往往还有
            //   handler 要挂上 —— 最典型是 async 函数抛错: 它返回的 promise 先被拒, 随后
            //   `.then` 的 thenable 采纳过程(js_promise_resolve_thenable_job)作为**下一个 job**
            //   才给它挂 handler。提前判定会把"正常接住的错误"当未处理, 让宿主收到运行时错误
            //   (与 Node 不一致)。真判据在循环外: 队列泵空了还留在表里的才算没人管。
        }
    }

    // 所有 pending job 处理完后, 若仍存在未处理的 promise 拒绝,
    // 仍要触发 Engine_error。否则 QuickJS 在"无更多 job 时才回调 rejection tracker"
    // 的实现下, m_promiseRejects 会被永久漏检, 引擎永远停在 running 态。
    if (hasPromiseReject())
        onExceptionOccurred(takePromiseRejectText());
}

bool JZJSEngine::hasPendingJob()
{
    if (!m_ctx)
        return false;

    // 外部模块挂起的原生异步任务(网络等): 结果还没回来就算有活
    if (m_pendingWork > 0)
        return true;

    // quickjs 微任务队列(setTimeout 0 触发的 promise 回调也先落在这里)
    if (JS_IsJobPending(m_rt))
        return true;

    // JS 侧 setTimeout/setInterval: 只要没 clear/没被触发移除就算 pending
    // (未到期的一次性任务要等 execOnce 到点才跑, 周期任务常驻会一直为 true)。
    // ⚠ 只算 **refed** 的: unref() 过的任务不吊住宿主(对齐 Node 的 Timeout.unref()),
    //   但它照样会触发 —— 只要本进程因别的原因还活着。
    for (int i = 0; i < (int)m_timeTasks.size(); i++)
    {
        if (m_timeTasks[i].refed)
            return true;
    }
    return false;
}

void JZJSEngine::addPendingWork()
{
    m_pendingWork++;
}

void JZJSEngine::releasePendingWork()
{
    m_pendingWork--;
    if (m_pendingWork < 0)
        m_pendingWork = 0;
}

int JZJSEngine::pendingWork() const
{
    return m_pendingWork;
}

// =========================================================================
// worker 线程表(进程级; 表与 id 分配都在主引擎上)
//
//   投递落点 = "目标 worker 的引擎"(JZObject, 活在那个 worker 的线程上):
//     jzApp->postEvent(目标引擎, 消息事件) -> 该引擎 customEvent
//       -> 它自己的 worker 模块 -> JS 的 onMessage
//   创建/回收只在主线程做: 线程对象必须建在将来 delete 它的那个线程上
//   (JZObject 析构会断言线程归属), 所以 spawnWorker 会把活转到主线程执行。
//
//   回收不用自定义事件: JZThread 自带 sigFinished(run() 返回后由框架 emit),
//   在这里 connect 到 onWorkerFinished 即可。
// =========================================================================
JZJSEngine::WorkerEntry::WorkerEntry()
{
    parentId = -1;
    ownerId = 0;
    exited = false;
    thread = nullptr;
    engine = nullptr;
}

void JZJSEngine::setWorkerModule(JZJSWorkerModule* module)
{
    m_workerModule = module;
}

JZJSWorkerModule* JZJSEngine::workerModule() const
{
    return m_workerModule;
}

void JZJSEngine::setWidgetModule(JZJSWidgetModule* module)
{
    m_widgetModule = module;
}

JZJSWidgetModule* JZJSEngine::widgetModule() const
{
    return m_widgetModule;
}

// 窗口宿主: "这台引擎里 new 出来的窗口归谁托管" (见 JZJSEngine.h / JZWindowHost.h)。
// 装上之后 JS 侧一行不用改 —— new JZWindow(...) 出来的就是宿主那边的窗口(如虚拟窗口)。
void JZJSEngine::setWindowHost(JZWindowHost* host)
{
    if (m_windowHost == host)
        return;
#ifdef HAS_JZWidget
    // 引擎已经起来了 (init 过): 立刻生效/还原 —— 宿主本身是进程级的, 与 init 无关。
    if (m_ctx)
    {
        if (host)
            jzSetWindowHost(host);
        else if (jzWindowHost() == m_windowHost)
            jzSetWindowHost(nullptr);   // 传 nullptr = 恢复平台默认(真窗口)
    }
#endif
    // 还没 init: 记下来, 由 init() 时装上 (见那里)
    m_windowHost = host;
}

int JZJSEngine::spawnWorker(const std::string& entry, const JZByteArray& workerData,
                            int ownerId, int parentId,
                            const std::string& workDir, const std::vector<std::string>& libraryDirs)
{
    // 表在主引擎上: 从哪个引擎调都统一落到它
    JZJSEngine* owner = (this == m_mainEngine) ? this : m_mainEngine;
    if (!owner)
        return -1;

    // 已在主线程: 直接建(线程对象归主线程, 将来才能由主线程 delete)
    if (owner->thread() == jzApp->currentThread())
        return owner->doSpawnWorker(entry, workerData, ownerId, parentId, workDir, libraryDirs);

    // 子 worker 里 new Worker 走这条: 投到主线程执行, 同步等结果
    int result = -1;
    JZSemaphore sem;
    runOnThread(owner->thread(), [&]() {
        result = owner->doSpawnWorker(entry, workerData, ownerId, parentId, workDir, libraryDirs);
        sem.release();
    });
    if (!sem.tryAcquire(1, 30000))
        return -1;      // 主线程忙过头(是延迟, 不是死锁)
    return result;
}

int JZJSEngine::doSpawnWorker(const std::string& entry, const JZByteArray& workerData,
                              int ownerId, int parentId,
                              const std::string& workDir, const std::vector<std::string>& libraryDirs)
{
    int id = m_nextWorkerId++;

    JZJSWorkerThread* t = new JZJSWorkerThread();
    // 最后一个实参 = 主引擎: worker 线程起来后要拿它 attachWorkerEngine(否则消息投不进来)。
    // doSpawnWorker 一定跑在主引擎上(spawnWorker 已经做过 this/mainEngine 的收敛)。
    t->setup(id, parentId, entry, workerData, workDir, libraryDirs, this);

    {
        JZMutexLocker lock(&m_workerMutex);
        WorkerEntry& e = m_workers[id];
        e.parentId = parentId;
        e.ownerId = ownerId;
        e.exited = false;
        e.thread = t;
        e.engine = nullptr;     // 等 worker 线程起来后 attachWorkerEngine
    }

    // 线程结束 -> 本引擎(主线程)回收。
    // JZThread::start 的线程体在 run() 返回后 emit sigFinished; 这里用"对象 + lambda"
    // 重载登记 recv = this, Auto 连接在 emit 时按 recv 所在线程判定 => 跨线程, 会排队到
    // 主线程执行(若改用只收 lambda 的重载则 hasRecv=false, 会在 worker 线程里同步跑,
    // 那就变成"在错误的线程 delete 线程对象"了)。
    connect(&t->sigFinished, this, [this, id]() { onWorkerFinished(id); });

    t->start();

    // 等"引擎 + 模块 + __jz 就绪"再返回, 之后 postToWorker 一定能命中。
    // (只等初始化, 不等入口脚本跑完 —— 脚本里顶层挂 onMessage 肯定早于收消息。)
    if (!t->waitReady(15000))
    {
        t->quit();              // 回收走正常的 Done 事件路径
        return -1;
    }
    return id;
}

bool JZJSEngine::postToWorker(int targetId, int fromId, const JZByteArray& data)
{
    JZJSEngine* owner = (this == m_mainEngine) ? this : m_mainEngine;
    if (!owner)
        return false;

    // 锁内投递: 与 detachWorkerEngine / reapWorker 互斥, 绝不会投给正在销毁的引擎。
    // 锁顺序 = 引擎表锁 -> 目标线程事件锁; 反向无人持线程锁来拿表锁, 不会死锁。
    JZMutexLocker lock(&owner->m_workerMutex);
    std::map<int, WorkerEntry>::iterator it = owner->m_workers.find(targetId);
    if (it == owner->m_workers.end() || it->second.exited)
        return false;

    if (!it->second.engine)
    {
        // worker 线程还没把引擎挂上: 先排队, attachWorkerEngine 时补投(见 WorkerEntry::pending)
        it->second.pending.push_back(std::make_pair(fromId, data));
        return true;
    }

    jzApp->postEvent(it->second.engine, new JZJSWorkerMessageEvent(fromId, data));
    return true;
}

void JZJSEngine::attachWorkerEngine(int id, JZJSEngine* engine)
{
    JZJSEngine* owner = (this == m_mainEngine) ? this : m_mainEngine;
    if (!owner)
        return;
    JZMutexLocker lock(&owner->m_workerMutex);
    std::map<int, WorkerEntry>::iterator it = owner->m_workers.find(id);
    if (it != owner->m_workers.end())
        it->second.engine = engine;
}

// 补投"引擎挂上之前"排队进来的消息(见 WorkerEntry::pending)。由 worker 线程在**入口脚本跑完
// 之后**调用 —— 入口脚本的第一件事通常是 __jz.worker.onMessage(...), 投早了会被当"没注册
// handler"丢掉(dropped message: __jz.worker.onMessage is not registered)。
void JZJSEngine::setMainEngine(JZJSEngine* main)
{
    m_mainEngine = main ? main : this;
}

void JZJSEngine::flushPendingWorkerMessages(int id)
{
    JZJSEngine* owner = (this == m_mainEngine) ? this : m_mainEngine;
    if (!owner)
        return;

    std::vector<std::pair<int, JZByteArray> > pending;
    JZJSEngine* target = nullptr;
    {
        JZMutexLocker lock(&owner->m_workerMutex);
        std::map<int, WorkerEntry>::iterator it = owner->m_workers.find(id);
        if (it == owner->m_workers.end() || !it->second.engine)
            return;
        target = it->second.engine;
        pending.swap(it->second.pending);
    }

    // 投递放在锁外: postEvent 自己会拿目标线程的事件锁, 这里不再需要引擎表锁
    for (size_t i = 0; i < pending.size(); i++)
        jzApp->postEvent(target, new JZJSWorkerMessageEvent(pending[i].first, pending[i].second));
}

void JZJSEngine::detachWorkerEngine(int id)
{
    JZJSEngine* owner = (this == m_mainEngine) ? this : m_mainEngine;
    if (!owner)
        return;
    JZMutexLocker lock(&owner->m_workerMutex);
    std::map<int, WorkerEntry>::iterator it = owner->m_workers.find(id);
    if (it != owner->m_workers.end())
    {
        it->second.engine = nullptr;    // 引擎即将销毁: 之后投递一律失败
        it->second.exited = true;
        it->second.pending.clear();     // 没投出去的消息随 worker 一起丢掉
    }
}

bool JZJSEngine::terminateWorker(int id)
{
    JZJSWorkerThread* t = nullptr;
    {
        JZJSEngine* owner = (this == m_mainEngine) ? this : m_mainEngine;
        if (!owner)
            return false;
        JZMutexLocker lock(&owner->m_workerMutex);
        std::map<int, WorkerEntry>::iterator it = owner->m_workers.find(id);
        if (it == owner->m_workers.end())
            return false;
        t = it->second.thread;
    }

    if (!t)
        return false;
    t->quit();      // 协作式: 事件循环退出后线程自己投 Done 事件走回收
    return true;
}

std::vector<int> JZJSEngine::workerIds()
{
    std::vector<int> out;
    JZJSEngine* owner = (this == m_mainEngine) ? this : m_mainEngine;
    if (!owner)
        return out;

    JZMutexLocker lock(&owner->m_workerMutex);
    for (std::map<int, WorkerEntry>::iterator it = owner->m_workers.begin(); it != owner->m_workers.end(); ++it)
        out.push_back(it->first);
    return out;
}

std::vector<int> JZJSEngine::workerChildIds(int parentId)
{
    std::vector<int> out;
    JZJSEngine* owner = (this == m_mainEngine) ? this : m_mainEngine;
    if (!owner)
        return out;

    JZMutexLocker lock(&owner->m_workerMutex);
    for (std::map<int, WorkerEntry>::iterator it = owner->m_workers.begin(); it != owner->m_workers.end(); ++it)
    {
        if (it->second.parentId == parentId)
            out.push_back(it->first);
    }
    return out;
}

int JZJSEngine::workerOwnerId(int id)
{
    JZMutexLocker lock(&m_workerMutex);
    std::map<int, WorkerEntry>::iterator it = m_workers.find(id);
    if (it == m_workers.end())
        return 0;
    return it->second.ownerId;
}

void JZJSEngine::reapWorker(int id)
{
    JZJSWorkerThread* t = nullptr;
    {
        JZMutexLocker lock(&m_workerMutex);
        std::map<int, WorkerEntry>::iterator it = m_workers.find(id);
        if (it == m_workers.end())
            return;                     // 已回收(幂等)
        t = it->second.thread;
        m_workers.erase(it);
    }

    if (t)
    {
        t->quit();                      // 正常已退出; 兜底请求一次
        t->wait();                      // join: 等线程真正结束
        delete t;                       // 必须在创建它的线程删(主线程)
    }
}

void JZJSEngine::onWorkerFinished(int id)
{
    // 已在主线程(sigFinished 跨线程排队到这里), 所以 join + delete 都是安全的线程归属
    int ownerId = workerOwnerId(id);

    // 退出码从线程对象上取(sigFinished 是无参信号): 0=正常 1=引擎初始化失败 2=入口脚本失败
    int code = 0;
    {
        JZMutexLocker lock(&m_workerMutex);
        std::map<int, WorkerEntry>::iterator it = m_workers.find(id);
        if (it != m_workers.end() && it->second.thread)
            code = it->second.thread->exitCode();
    }

    // 1. 级联: 先终止它名下的子 worker(子各自会再 emit 一次 sigFinished -> 各自回收)
    std::vector<int> kids = workerChildIds(id);
    for (size_t i = 0; i < kids.size(); i++)
        terminateWorker(kids[i]);

    // 2. 回收线程对象(join + delete + 摘表)
    reapWorker(id);

    // 3. 通知 owner 引擎跑 JS 侧 'exit' 回调(锁内投递, 不投给正在销毁的引擎)
    JZMutexLocker lock(&m_workerMutex);
    std::map<int, WorkerEntry>::iterator it = m_workers.find(ownerId);
    if (it != m_workers.end() && !it->second.exited && it->second.engine)
        jzApp->postEvent(it->second.engine, new JZJSWorkerExitEvent(id, code));
}

void JZJSEngine::reapAllWorkers()
{
    std::vector<int> ids;
    {
        JZMutexLocker lock(&m_workerMutex);
        for (std::map<int, WorkerEntry>::iterator it = m_workers.begin(); it != m_workers.end(); ++it)
        {
            if (it->first != 0)
                ids.push_back(it->first);
        }
    }

    // 先全部请求退出(避免"父等子"的顺序依赖), 再逐个 wait + delete
    for (size_t i = 0; i < ids.size(); i++)
    {
        JZJSWorkerThread* t = nullptr;
        {
            JZMutexLocker lock(&m_workerMutex);
            std::map<int, WorkerEntry>::iterator it = m_workers.find(ids[i]);
            if (it != m_workers.end())
                t = it->second.thread;
        }
        if (t)
            t->quit();
    }
    for (size_t i = 0; i < ids.size(); i++)
        reapWorker(ids[i]);
}