// ---------------------------------------------------------------------------
// AlarmPlayer.js —— 对应 C++ BTEServer/AlarmPlayer.h + AlarmPlayer.cpp
//
// 报警音循环播放 (Windows PlaySound)。
//
// Qt -> JS: QString.utf16() -> JS 字符串; Win32 PlaySoundW 保留原样调用
//           (SND_FILENAME | SND_LOOP | SND_ASYNC | SND_NODEFAULT)。
//
// 2026-09-14: 去 Qt 兼容层 —— NULL/FALSE 用 JS 的 null/false, SND_* 是 Win32 原始
//             常量, 只有本文件用, 直接写在这里(原来是 qtcompat 导出的)。
// ---------------------------------------------------------------------------

const SND_ASYNC = 0x0001;
const SND_NODEFAULT = 0x0002;
const SND_LOOP = 0x0008;
const SND_FILENAME = 0x00020000;

export class AlarmPlayer
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // AlarmPlayer();
    constructor()
    {
        this.m_playing = false;
    }

    // ~AlarmPlayer();
    dispose()
    {
        this.stop();
    }

    // bool start(const QString &wavPath);
    start(wavPath)
    {
        if (this.m_playing)
            return true;

        // SND_ASYNC : return immediately, keep playing in the background
        // SND_LOOP  : repeat until stopped
        // SND_NODEFAULT : stay silent instead of the system default beep if missing
        const ok = PlaySoundW(wavPath, null,
            SND_FILENAME | SND_LOOP | SND_ASYNC | SND_NODEFAULT);

        // 原 C++ 是 `ok != FALSE`(BOOL 非 0 即真); 绑定层给回来的是 JS 布尔,
        // 所以在这里用 !! 归一(原来那套 `!== 0` 对 false 会判成真)。
        this.m_playing = !!ok;
        return this.m_playing;
    }

    // void stop();
    stop()
    {
        if (!this.m_playing)
            return;

        PlaySoundW(null, null, 0);
        this.m_playing = false;
    }

    // bool isPlaying() const { return m_playing; }
    isPlaying()
    {
        return this.m_playing;
    }
}
