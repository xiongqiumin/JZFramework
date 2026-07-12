#include "main_window.h"
#include "JZDebug.h"
#include "lvgl.h"
#include <cstring>
#include <cmath>

MainWindow::MainWindow()    
    :m_wsConnected(false)
    ,m_isRecording(false)
    ,m_recordBytes(0)
{
    setupUi();
    resize(320,240);    

    // ---- 内存监控 ----
    m_memTimer = new JZTimer(this);
    m_memTimer->setSingleShot(false);
    connect(&m_memTimer->sigTimeout, this, &MainWindow::updateMemLabel);
    m_memTimer->start(1000);
    updateMemLabel();

    // ---- WebSocket (WiFi 连上后再连) ----
    m_wsClient = new JZWebSocket(this);
    connect(&m_wsClient->sigConnected,              this, &MainWindow::wsOnConnected);
    connect(&m_wsClient->sigDisconnected,           this, &MainWindow::wsOnDisconnected);
    connect(&m_wsClient->sigBinaryMessageReceived,  this, &MainWindow::wsOnBinaryMessage);
}

MainWindow::~MainWindow()
{    
    m_wsClient->disconnectFromHost();
}

// ==================== UI ====================

void MainWindow::setupUi()
{
    // 状态标签: 屏幕中央
    m_statusLabel = new JZLabel(this);
    m_statusLabel->setAlign(LV_TEXT_ALIGN_CENTER);
    lv_obj_align(m_statusLabel->handle(), LV_ALIGN_CENTER, 0, 0);
    m_statusLabel->setText("Hold KEY0 to talk");

    // WiFi 标签: 右上角
    m_wifiLabel = new JZLabel(this);
    lv_obj_align(m_wifiLabel->handle(), LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_text_font(m_wifiLabel->handle(), &lv_font_montserrat_14, 0);
    m_wifiLabel->setText("WiFi: Off");

    // 内存标签: 右下角
    m_memLabel = new JZLabel(this);
    lv_obj_align(m_memLabel->handle(), LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    lv_obj_set_style_text_font(m_memLabel->handle(), &lv_font_montserrat_14, 0);
}

// ==================== WiFi ====================

void MainWindow::onWifiKeyPressed()
{
    jzDebug() << "MainWindow: KEY2 pressed, start scan and connect";
    m_wifiLabel->setText("WiFi: Scanning...");
    m_statusLabel->setText("Scanning...");
}

void MainWindow::onWifiConnecting(const std::string &ssid)
{
    jzDebug() << "MainWindow: WiFi connecting to" << ssid;
    m_wifiLabel->setText("WiFi: " + ssid);
}

void MainWindow::onWifiConnected(const std::string &ssid)
{
    jzDebug() << "MainWindow: WiFi connected to" << ssid;
    m_wifiLabel->setText("WiFi: " + ssid);

}

void MainWindow::onWifiConnectError(int code, const std::string &message)
{
    jzDebug() << "MainWindow: WiFi connected error code" << code << ", message" << message;
}

// ==================== WebSocket ====================

void MainWindow::wsOnConnected()
{
}

void MainWindow::wsOnDisconnected()
{
    jzDebug() << "MainWindow: WS disconnected";
    m_wsConnected = false;
    m_statusLabel->setText("WS Disconnected");
}

void MainWindow::wsOnBinaryMessage(const JZByteArray &data)
{
}

void MainWindow::onPlayFinish()
{
    jzDebug() << "MainWindow: play finish";
    m_statusLabel->setText("Hold KEY0 to talk");
}

// ==================== 按键 ====================

void MainWindow::onBtnPressed()
{
    m_statusLabel->setText("Sending...");
}

void MainWindow::onBtnReleased()
{
    jzDebug() << "MainWindow: button released";
    if (!m_isRecording) return;
    m_isRecording = false;
    m_statusLabel->setText("Hold KEY0 to talk");
}

void MainWindow::onAudioDataReady(const JZByteArray &data)
{
    if (!m_isRecording || !m_wsConnected) return;

    m_recordBytes += data.size();
    m_wsClient->sendBinaryMessage(data);
}

// ==================== 内存 ====================

void MainWindow::updateMemLabel()
{
    m_memLabel->setText("m buf");
}
