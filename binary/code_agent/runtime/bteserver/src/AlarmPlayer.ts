import { FALSE, NULL, SND_ASYNC, SND_FILENAME, SND_LOOP, SND_NODEFAULT } from './qtcompat.js';
// ---------------------------------------------------------------------------
// AlarmPlayer.js —— 对应 C++ BTEServer/AlarmPlayer.h + AlarmPlayer.cpp
//
// 报警音循环播放 (Windows PlaySound)。
//
// Qt -> JS: QString.utf16() -> JS 字符串; Win32 PlaySoundW 保留原样调用
//           (SND_FILENAME | SND_LOOP | SND_ASYNC | SND_NODEFAULT)。
// ---------------------------------------------------------------------------

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
        const ok = PlaySoundW(wavPath, NULL,
            SND_FILENAME | SND_LOOP | SND_ASYNC | SND_NODEFAULT);

        this.m_playing = (ok !== FALSE);
        return this.m_playing;
    }

    // void stop();
    stop()
    {
        if (!this.m_playing)
            return;

        PlaySoundW(NULL, NULL, 0);
        this.m_playing = false;
    }

    // bool isPlaying() const { return m_playing; }
    isPlaying()
    {
        return this.m_playing;
    }
}
