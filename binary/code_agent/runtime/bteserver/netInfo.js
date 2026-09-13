// ---------------------------------------------------------------------------
// net/netInfo.js —— 对应 C++ common/net/netInfo.h (netInfo.cpp 为空文件)
//
// class NetInfo { QString ip; int port; };
// Qt -> JS: QString -> string。netInfo.h 里没有 #include <QString>(靠别处间接引入),
// JS 侧不涉及。
// ---------------------------------------------------------------------------

export class NetInfo
{
    constructor()
    {
        this.ip = "";
        this.port = 0;
    }
}
