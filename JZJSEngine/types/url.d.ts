// 全局 URL / URLSearchParams 见 globals.d.ts; 这里只转发 'url' 模块(parse)。
declare module 'url' {
    export * from '../js_wrapper/net/types/url';
}
