#pragma once

#include "JZLabel.h"
#include "JZWidget.h"
#include "JZWebSocket.h"
#include "JZTimer.h"

class MainWindow : public JZWidget
{
public:
    MainWindow();
    ~MainWindow();

private:
    void setupUi();

    // 按键
    void onBtnPressed();
    void onBtnReleased();
    void onWifiKeyPressed();
    void onAudioDataReady(const JZByteArray &data);

    // WiFi
    void onWifiConnecting(const std::string &ssid);
    void onWifiConnected(const std::string &ssid);
    void onWifiConnectError(int code, const std::string &message);
    
    // WebSocket
    void wsOnConnected();
    void wsOnDisconnected();
    void wsOnBinaryMessage(const JZByteArray &data);

    // 播放
    void onPlayFinish();

    // 内存
    void updateMemLabel();
    
    JZLabel      *m_statusLabel;  // 状态(屏幕中央)
    JZLabel      *m_wifiLabel;    // WiFi状态(右上角)
    JZLabel      *m_memLabel;     // 内存(右下角)
    JZTimer      *m_memTimer;

    JZWebSocket              *m_wsClient;    

    bool m_wsConnected;
    bool m_isRecording;
    int  m_recordBytes;
};
