// Copyright (C) 2011-2012 Denis Shienkov <denis.shienkov@gmail.com>
// Copyright (C) 2011 Sergey Belyashov <Sergey.Belyashov@gmail.com>
// Copyright (C) 2012 Laszlo Papp <lpapp@kde.org>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef JZSerialPort_P_H
#define JZSerialPort_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <memory>
#include JZSerialPort
#include "JZGlobal.h"
#include "JZTimer.h"

#if defined(WIN32)
#  include <windows.h>
#elif defined(Q_OS_UNIX)
#  include <limits.h>
#  include <termios.h>
#  ifdef Q_OS_ANDROID
struct serial_struct {
    int     type;
    int     line;
    unsigned int    port;
    int     irq;
    int     flags;
    int     xmit_fifo_size;
    int     custom_divisor;
    int     baud_base;
    unsigned short  close_delay;
    char    io_type;
    char    reserved_char[1];
    int     hub6;
    unsigned short  closing_wait;
    unsigned short  closing_wait2;
    unsigned char   *iomem_base;
    unsigned short  iomem_reg_shift;
    unsigned int    port_high;
    unsigned long   iomap_base;
};
#    define ASYNC_SPD_CUST  0x0030
#    define ASYNC_SPD_MASK  0x1030
#    define PORT_UNKNOWN    0
#  elif defined(Q_OS_LINUX)
#    include <linux/serial.h>
#  endif
#else
#  error Unsupported OS
#endif

#ifndef JZSerialPort_BUFFERSIZE
#define JZSerialPort_BUFFERSIZE 32768
#endif

class QWinOverlappedIoNotifier;
class JZTimer;
class QSocketNotifier;

#if defined(Q_OS_UNIX)
std::string serialPortLockFilePath(const std::string &portName);
#endif

class JZSerialPortErrorInfo
{
public:
    JZSerialPortErrorInfo(JZSerialPort::SerialPortError newErrorCode = JZSerialPort::UnknownError,
                                  const std::string &newErrorString = std::string());
    JZSerialPort::SerialPortError errorCode = JZSerialPort::UnknownError;
    std::string errorString;
};

class JZSerialPortPrivate
{
    JZSerialPort *q;

public:
    JZSerialPortPrivate();

    bool open();
    void close();

    bool setDataTerminalReady(bool set);
    bool setRequestToSend(bool set);

    bool flush();
    bool clear();

    bool sendBreak(int duration);
    bool setBreakEnabled(bool set);

    bool waitForReadyRead(int msec);
    bool waitForBytesWritten(int msec);

    bool setBaudRate(int32_t baudRate);
    bool setDataBits(JZSerialPort::DataBits dataBits);
    bool setParity(JZSerialPort::Parity parity);
    bool setStopBits(JZSerialPort::StopBits stopBits);
    bool setFlowControl(JZSerialPort::FlowControl flowControl);

    JZSerialPortErrorInfo getSystemError(int systemErrorCode = -1) const;

    void setError(const JZSerialPortErrorInfo &errorInfo);

    int64_t writeData(const char *data, int64_t maxSize);

    bool initialize();

    static std::vector<int32_t> standardBaudRates();

    int64_t readBufferMaxSize = 0;
    int64_t writeBufferMaxSize = 0;

    std::string systemLocation;
    int32_t inputBaudRate = JZSerialPort::Baud9600;
    int32_t outputBaudRate = JZSerialPort::Baud9600;

    bool settingsRestoredOnClose = true;
    bool startAsyncRead();
    bool emittedReadyRead = false;

    int baudRate;
    JZSerialPort::DataBits dataBits;
    JZSerialPort::Parity parity;
    JZSerialPort::StopBits stopBits;
    JZSerialPort::FlowControl flowControl;
    JZSerialPort::SerialPortError error;
    JZByteArray buffer;
    JZByteArray writeBuffer;

#if defined(WIN32)

    bool setDcb(DCB *dcb);
    bool getDcb(DCB *dcb);
    //OVERLAPPED *waitForNotified(QDeadlineTimer deadline);

    int64_t queuedBytesCount(JZSerialPort::Direction direction) const;

    bool completeAsyncCommunication(int64_t bytesTransferred);
    bool completeAsyncRead(int64_t bytesTransferred);
    bool completeAsyncWrite(int64_t bytesTransferred);

    bool startAsyncCommunication();
    bool _q_startAsyncWrite();
    void _q_notified(DWORD numberOfBytes, DWORD errorCode, OVERLAPPED *overlapped);

    void emitReadyRead();

    DCB restoredDcb;
    COMMTIMEOUTS currentCommTimeouts;
    COMMTIMEOUTS restoredCommTimeouts;
    HANDLE handle = INVALID_HANDLE_VALUE;
    JZByteArray readChunkBuffer;
    JZByteArray writeChunkBuffer;
    bool communicationStarted = false;
    bool writeStarted = false;
    bool readStarted = false;
    QWinOverlappedIoNotifier *notifier = nullptr;
    JZTimer *startAsyncWriteTimer = nullptr;
    OVERLAPPED communicationOverlapped;
    OVERLAPPED readCompletionOverlapped;
    OVERLAPPED writeCompletionOverlapped;
    DWORD triggeredEventMask = 0;

#elif defined(Q_OS_UNIX)

    static int32_t settingFromBaudRate(int32_t baudRate);

    bool setTermios(const termios *tio);
    bool getTermios(termios *tio);

    bool setCustomBaudRate(int32_t baudRate, JZSerialPort::Directions directions);
    bool setStandardBaudRate(int32_t baudRate, JZSerialPort::Directions directions);

    bool isReadNotificationEnabled() const;
    void setReadNotificationEnabled(bool enable);
    bool isWriteNotificationEnabled() const;
    void setWriteNotificationEnabled(bool enable);

    bool waitForReadOrWrite(bool *selectForRead, bool *selectForWrite,
                            bool checkRead, bool checkWrite,
                            int msecs);

    int64_t readFromPort(char *data, int64_t maxSize);
    int64_t writeToPort(const char *data, int64_t maxSize);

#ifndef CMSPAR
    int64_t writePerChar(const char *data, int64_t maxSize);
#endif

    bool readNotification();
    bool startAsyncWrite();
    bool completeAsyncWrite();
    void handleException();

    struct termios restoredTermios;
    int descriptor = -1;

    QSocketNotifier *readNotifier = nullptr;
    QSocketNotifier *writeNotifier = nullptr;
    QSocketNotifier *exceptionNotifier = nullptr;

    bool readPortNotifierCalled = false;
    bool readPortNotifierState = false;
    bool readPortNotifierStateSet = false;

    bool emittedBytesWritten = false;

    int64_t pendingBytesWritten = 0;
    bool writeSequenceStarted = false;

    bool gotException = false;

    std::unique_ptr<QLockFile> lockFileScopedPointer;

#endif
};

#endif // JZSerialPort_P_H
