// ---------------------------------------------------------------------------
// net/netInfo.js —— 对应 C++ common/net/netInfo.h (netInfo.cpp 为空文件)
//
// class NetInfo { QString ip; int port; };
// Qt -> JS: QString -> string。netInfo.h 里没有 #include <QString>(靠别处间接引入),
// JS 侧不涉及。
// ---------------------------------------------------------------------------

export class NetInfo
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor()
    {
        this.ip = "";
        this.port = 0;
    }
}
