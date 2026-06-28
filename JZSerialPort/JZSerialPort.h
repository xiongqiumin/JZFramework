// Copyright (C) 2012 Denis Shienkov <denis.shienkov@gmail.com>
// Copyright (C) 2013 Laszlo Papp <lpapp@kde.org>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef QSERIALPORT_H
#define QSERIALPORT_H

#include "JZObject.h"
#include "JZSerialPortInfo.h"

class JZSerialPortPrivate;
class JZSerialPort : public JZObject
{
#if defined(WIN32)
    typedef void* Handle;
#else
    typedef int Handle;
#endif

public:

    enum Direction  {
        Input = 1,
        Output = 2,
        AllDirections = Input | Output
    };

    enum BaudRate {
        Baud1200 = 1200,
        Baud2400 = 2400,
        Baud4800 = 4800,
        Baud9600 = 9600,
        Baud19200 = 19200,
        Baud38400 = 38400,
        Baud57600 = 57600,
        Baud115200 = 115200
    };

    enum DataBits {
        Data5 = 5,
        Data6 = 6,
        Data7 = 7,
        Data8 = 8
    };
    
    enum Parity {
        NoParity = 0,
        EvenParity = 2,
        OddParity = 3,
        SpaceParity = 4,
        MarkParity = 5
    };

    enum StopBits {
        OneStop = 1,
        OneAndHalfStop = 3,
        TwoStop = 2
    };

    enum FlowControl {
        NoFlowControl,
        HardwareControl,
        SoftwareControl
    };

    enum PinoutSignal {
        NoSignal = 0x00,
        DataTerminalReadySignal = 0x04,
        DataCarrierDetectSignal = 0x08,
        DataSetReadySignal = 0x10,
        RingIndicatorSignal = 0x20,
        RequestToSendSignal = 0x40,
        ClearToSendSignal = 0x80,
        SecondaryTransmittedDataSignal = 0x100,
        SecondaryReceivedDataSignal = 0x200
    };

    enum SerialPortError {
        NoError,
        DeviceNotFoundError,
        PermissionError,
        OpenError,
        WriteError,
        ReadError,
        ResourceError,
        UnsupportedOperationError,
        UnknownError,
        TimeoutError,
        NotOpenError
    };

    explicit JZSerialPort(JZObject *parent = nullptr);
    explicit JZSerialPort(const std::string &name, JZObject *parent = nullptr);
    explicit JZSerialPort(const JZSerialPortInfo &info, JZObject *parent = nullptr);
    virtual ~JZSerialPort();

    void setPortName(const std::string &name);
    std::string portName() const;

    void setPort(const JZSerialPortInfo &info);

    bool open();
    void close();
    bool isOpen();

    bool setBaudRate(int32_t baudRate);
    int32_t baudRate() const;

    bool setDataBits(DataBits dataBits);
    DataBits dataBits() const;

    bool setParity(Parity parity);
    Parity parity() const;

    bool setStopBits(StopBits stopBits);
    StopBits stopBits() const;

    bool setFlowControl(FlowControl flowControl);
    FlowControl flowControl() const;

    bool setDataTerminalReady(bool set);
    bool isDataTerminalReady();

    bool setRequestToSend(bool set);
    bool isRequestToSend();

    bool flush();
    void clear();

    SerialPortError error() const;
    void clearError();

    int64_t readBufferSize() const;
    void setReadBufferSize(int64_t size);

    int64_t writeBufferSize() const;
    void setWriteBufferSize(int64_t size);

    bool isSequential() const;

    int64_t bytesAvailable() const;
    int64_t bytesToWrite() const;
    bool canReadLine() const;

    bool waitForReadyRead(int msecs = 30000);
    bool waitForBytesWritten(int msecs = 30000);

    bool setBreakEnabled(bool set = true);
    bool isBreakEnabled() const;

    bool settingsRestoredOnClose() const;
    void setSettingsRestoredOnClose(bool restore);

    Handle handle() const;

jz_signals: 
    JZSigVoid sigReadyRead;

protected:
    int64_t readData(char *data, int64_t maxSize);
    int64_t readLineData(char *data, int64_t maxSize);
    int64_t writeData(const char *data, int64_t maxSize);
    JZSerialPortPrivate *d;

private:
    bool isReadable();
    bool isWritable();

    JZ_DISABLE_COPY(JZSerialPort)
};

#endif // QSERIALPORT_H
