/// <reference path="../../types/events.d.ts" />

/**
 * @jz/agent —— 外部定义 (external definition, public contract)
 *
 * JZAgent 的 JS 封装。运行时由引擎加载 dist/index.js 提供,
 * 底层是 JZAgent(JZAgent/JZAgent.{h,cpp}) 及其配套 JZAgentTool / JZMcpClient,
 * 经某 JZJSAgentModule 注册的 `__jz.agent.*` 原子 API 暴露。
 * 与包内实现 index.ts 配套。
 *
 * Agent 是事件驱动的异步对象:
 *   - request(text) 发出用户消息, 之后通过 'reply' / 'thinking' / 'toolcall' /
 *     'toolresult' / 'error' / 'finished' / 'history' 事件上抛增量;
 *   - 工具既可用 JS 函数注册(addTool), 也可接远程 MCP 工具(addMcp);
 *   - 同一时刻只有一轮对话在跑(Idle 之后才能 reset 重开)。
 *
 * 运行期按工程内的相对路径 import 运行时 JS:
 *   import { Agent, McpClient } from '../js_wrapper/agent/dist/index.js';
 * 类型按包名引用:
 *   import type { Agent, AgentConfig, LLMContext } from '@jz/agent';
 */

/** JSON Schema 子集里的参数类型(与 C++ ParamType 一一对应) */
export type ParamTypeName = 'string' | 'number' | 'boolean' | 'array' | 'object' | 'null';

/** 工具参数的 JSON Schema 子集(对齐 OpenAI function calling 的 parameters) */
export interface ParamSchema {
    /** 参数名 */
    name: string;
    /** 参数描述 */
    description: string;
    /** 参数类型 */
    type: ParamTypeName;
    /** 默认值约束 */
    default?: unknown;
    /** 字符串最小长度 */
    minLength?: number;
    /** 字符串最大长度 */
    maxLength?: number;
    /** 数值下限(含) */
    minimum?: number;
    /** 数值上限(含) */
    maximum?: number;
    /** 是否排除下限 */
    exclusiveMinimum?: boolean;
    /** 是否排除上限 */
    exclusiveMaximum?: boolean;
    /** 当 type === 'array' 时, 数组元素的类型 */
    array_item_type?: ParamTypeName;
    /** 当 type === 'object' 时, 子字段 schema */
    properties?: ParamSchema[];
    /** 枚举约束 */
    enum_values?: string[];
    /** 格式约束(如 'date-time' / 'email' / 'uri') */
    format?: string;
    /** 正则约束 */
    pattern?: string;
}

/** 一次工具调用请求(OpenAI 标准结构) */
export interface ToolCall {
    /** 调用 id */
    id: string;
    /** 调用类型 */
    type: string;
    /** 被调用的函数名 */
    function_name: string;
    /** 原始函数参数(JSON 字符串) */
    function_args: string;
    /** function_args 解析后的 JSON 对象 */
    params: any;
}

/** 一次工具调用的结果 */
export interface ToolCallResult {
    /** 对应 ToolCall.id */
    id: string;
    /** 是否执行成功 */
    isOk: boolean;
    /** 结果文本 */
    result: string;
}

/** 工具定义(OpenAI 标准结构) */
export interface ToolDefine {
    /** 工具名 */
    name: string;
    /** 工具描述 */
    description: string;
    /** 参数 schema 列表 */
    parameters: ParamSchema[];
    /** 必填参数名 */
    required_parameters: string[];
}

/** 技能定义 */
export interface SkillDefine {
    /** 技能名 */
    name: string;
    /** 技能提示 */
    tips: string;
    /** 技能用法说明 */
    usage: string;
}

/**
 * 一轮(或累计)的 token 用量。
 * 兼容 OpenAI 与 llama.cpp 两种写法: 后者用 prompt_n/predicted_n/cache_n 报 token,
 * 并额外给 timings 计时(下面 4 个字段, 其他 provider 恒为 0)。
 * 计时只存原始量, 速率请按 Σtoken / Σms 现算(多轮求和无意义)。
 */
export interface TokenUsage {
    /** 本次输入 token */
    prompt_tokens: number;
    /** 本次输出 token */
    completion_tokens: number;
    /** 本次总计 */
    total_tokens: number;
    /** 本次从 KV 缓存命中的输入 token(前缀缓存命中) */
    cached_tokens: number;
    /** llama.cpp: 本次 prefill(输入)耗时(ms) */
    prompt_ms: number;
    /** llama.cpp: 本次生成(输出)耗时(ms) */
    predicted_ms: number;
    /** llama.cpp: 投机采样/MTP 草稿 token 数 */
    draft_n: number;
    /** llama.cpp: 投机采样/MTP 被接受数 */
    draft_n_accepted: number;
}

/** 历史消息角色 */
export type LLMRole = 'user' | 'assistant' | 'system' | 'tool';

/**
 * 历史消息性质:
 *   - 'message'      : 普通对话消息
 *   - 'contextstart' : 上下文起点(用户输入的第一条消息, 界面照常显示)
 *   - 'summary'      : 压缩摘要(内部, 不进界面, 但同样是新的上下文起点)
 */
export type LLMKind = 'message' | 'contextstart' | 'summary';

/** 单条历史消息 */
export interface LLMHistory {
    role: LLMRole;
    kind: LLMKind;
    /** 文本内容 */
    content: string;
    /** 推理/思考过程(实时上抛, 不回写模型上下文) */
    thinking: string;
    /** 附带的图片(data URL), 多模态消息用 */
    images: string[];
    /** 工具请求 */
    toolRequest: ToolCall[];
    /** 工具返回 id */
    toolResponseId: string;
    /** 本轮 token 增量(仅 assistant 回复由响应 usage 填充) */
    usage: TokenUsage;
}

/** 整体对话上下文(上下文存取 / 存档用) */
export interface LLMContext {
    /** 历史消息(共享指针, 按只读使用) */
    history: LLMHistory[];
    /** 累计 token = sum(history[i].usage) */
    usage: TokenUsage;
}

/** token 超预算处理策略 */
export type TokenStrategy = 'error' | 'compress';

/** JZAgent 初始化配置(对应 C++ JZAgentConfig) */
export interface AgentConfig {
    /** 工作根目录(技能/缓存等相对路径基准) */
    rootPath?: string;
    /** 模型接口地址 */
    modelUrl: string;
    /** 模型名 */
    modelName: string;
    /** 模型 token(鉴权) */
    modelToken: string;
    /** 网络请求超时(毫秒), 默认 120000 */
    netTimeout?: number;
    /** 是否复用 TCP 连接(keep-alive), 默认 true */
    reuseConnection?: boolean;
    /** 最大对话轮次, -1 代表不限制, 默认 100 */
    maxRounds?: number;
    /** 单次任务 token 预算上限, 默认 100000 */
    maxToken?: number;
    /** token 超限策略, 默认 'error' */
    tokenStrategy?: TokenStrategy;
    /** 附加到聊天请求体的自定义字段(对接各家私有参数), 与框架生成字段同名时以此为准 */
    extraBody?: any;
}

/** JZResult<SkillDefine> 的 JS 镜像(loadSkill 返回) */
export interface SkillLoadResult {
    /** 是否成功 */
    ok: boolean;
    /** 成功时的技能定义 */
    value?: SkillDefine;
    /** 失败时的错误文本 */
    error?: string;
    /** 失败时的错误码 */
    code?: number;
}

/**
 * JS 侧工具。call() 返回字符串结果(或 Promise<string>), 异常会被捕获并作为工具失败回传模型。
 * 注册后由引擎桥接为一个 JZAgentTool; 模型请求调用时, 引擎传入已解析的参数对象(args)。
 */
export interface AgentTool {
    /** 工具名(同 agent 内须唯一) */
    name: string;
    /** 工具描述 */
    description: string;
    /** 参数 schema 列表 */
    parameters?: ParamSchema[];
    /** 必填参数名 */
    required_parameters?: string[];
    /** 同步或异步执行; 返回值作为工具结果文本 */
    call(args: any): string | Promise<string>;
}

/**
 * LLM 智能体。事件驱动的异步对话对象。
 *
 * 用法:
 *   const agent = new Agent();
 *   agent.init({ modelUrl, modelName, modelToken });
 *   agent.setSystemPrompt('你是一个助手');
 *   agent.addTool({ name: 'get_time', description: '...', call: () => new Date().toString() });
 *   agent.on('reply', (text) => process.stdout.write(text));
 *   agent.on('finished', () => console.log('done'));
 *   agent.request('现在几点了?');
 */
export class Agent extends EventEmitter {
    constructor();

    /** 用配置初始化(须在 request 之前调用一次) */
    init(config: AgentConfig): void;

    /** 设置系统提示(仅 Inactive 状态可调用) */
    setSystemPrompt(prompt: string): void;
    /** 当前系统提示 */
    getSystemPrompt(): string;
    /** 取"系统提示 + 技能"拼好的完整系统提示文本 */
    dumpSystemPrompt(): string;
    /** 取工具汇总提示文本 */
    dumpToolsPrompt(): string;

    /** 注册一个 JS 工具(函数式, 由引擎桥接为 JZAgentTool) */
    addTool(tool: AgentTool): void;
    /** 按名字取已注册工具定义; 不存在返回 null */
    tool(name: string): ToolDefine | null;

    /** 接入一个已连接就绪的 MCP 客户端(agent 接管其所有权, 并把远端工具登记进来) */
    addMcp(client: McpClient): void;

    /** 流式回复是否开启 */
    setStreamEnabled(enabled: boolean): void;
    isStreamEnabled(): boolean;
    /** 思考过程是否开启 */
    setThinkEnabled(enabled: boolean): void;

    // ---- 对话 ----
    /** 发送一条纯文本用户消息 */
    request(prompt: string): void;
    /** 发送带单张图片的多模态用户消息; image 为 http(s) 地址 / data URL / base64 文本 */
    request(text: string, image: string): void;
    /** 发送带多张图片的多模态用户消息; 每个元素为地址或 data URL */
    request(text: string, images: string[]): void;

    /** 终止当前对话(正在等待回复时中止在途请求) */
    stop(): void;
    /**
     * 一轮对话结束(Idle)后重开新任务: 重置回初始态, 清空对话历史/用量/待发请求,
     * 保留 system prompt / tools / skills 配置。
     */
    reset(): void;

    // ---- 技能 ----
    /** 注册一个技能定义 */
    addSkill(skill: SkillDefine): void;
    /** 按名字取技能定义; 不存在返回 null */
    skill(name: string): SkillDefine | null;
    /** 从磁盘加载技能定义(json 文件) */
    loadSkill(name: string, path: string): SkillLoadResult;

    // ---- 上下文存取 ----
    /** 导出当前上下文(浅拷贝, history 内消息对象与 agent 共享, 按只读使用) */
    saveContext(): LLMContext;
    /** 载入上下文(整体替换当前对话); 传入后不要再改动 */
    loadContext(ctx: LLMContext): void;

    /** 最近一次错误文本(无错误返回 '') */
    errorString(): string;

    // ---- 事件重载 ----
    on(event: 'reply', listener: (text: string) => void): this;
    on(event: 'thinking', listener: (text: string) => void): this;
    on(event: 'toolcall', listener: (call: ToolCall) => void): this;
    on(event: 'toolresult', listener: (result: ToolCallResult) => void): this;
    on(event: 'error', listener: (err: Error) => void): this;
    on(event: 'finished', listener: () => void): this;
    /** 历史新增了一条消息(增量, 追加到末尾) */
    on(event: 'history', listener: (history: LLMHistory) => void): this;
    on(event: string, listener: (...args: any[]) => void): this;
}

/** MCP 客户端传输方式 */
export type McpTransport = 'none' | 'process' | 'http';

/**
 * MCP 客户端。连接一个 MCP 服务器(stdio 子进程 / Streamable HTTP),
 * 拉取远端工具列表, 每个工具在 agent 侧成为可登记的本地代理工具。
 *
 * 三个 startXxx 都是异步发起(只返回"是否成功发起"), 连接流程结束会发 'finished' 事件;
 * 结束后用 isReady() / errorString() 判断结果。请先 connect 好 'finished' 再调用 startXxx,
 * 或用 waitForFinished() 同步等待。
 */
export class McpClient extends EventEmitter {
    constructor();

    // ---- 建立连接(异步发起) ----
    /** stdio: 启动子进程, 通过 stdin/stdout 收发 JSON-RPC(一行一条) */
    startProcess(program: string, arguments: string[], timeoutMs?: number): boolean;
    /** Streamable HTTP: 每个请求一次 POST, 响应支持 JSON 与 SSE 两种格式 */
    startHttp(url: string, timeoutMs?: number): boolean;

    /** 关闭连接并清空远端工具 */
    close(): void;
    /** 阻塞直到本次连接流程结束(依赖事件循环, 须在主线程调用) */
    waitForFinished(): void;

    /** 连接 + 工具列表是否已就绪 */
    isReady(): boolean;
    /** 当前传输方式 */
    transport(): McpTransport;
    /** 最近一次错误文本 */
    errorString(): string;

    // ---- 远端工具对应的本地代理工具 ----
    /** 本地代理工具名列表 */
    tools(): string[];
    /** 按本地工具名取代理工具定义; 不存在返回 null */
    tool(name: string): ToolDefine | null;

    /** 本地工具名前缀, 默认 '<serverName>__'; 显式设置时以设置为准 */
    setToolPrefix(prefix: string): void;
    /** 当前本地工具名前缀 */
    toolPrefix(): string;

    /** 单次 tools/call 超时(毫秒), 默认 120000 */
    setCallTimeout(ms: number): void;
    callTimeout(): number;

    /** 服务器名 */
    serverName(): string;
    /** 服务器版本 */
    serverVersion(): string;

    /** 本地工具名 <-> 远端工具名 映射 */
    makeLocalName(remoteName: string): string;
    remoteToolName(localName: string): string;

    // ---- 事件重载 ----
    /** 一次连接流程结束: 成功(isReady()==true)或失败 */
    on(event: 'finished', listener: () => void): this;
    on(event: string, listener: (...args: any[]) => void): this;
}

export default Agent;
