// Copyright (C) 2012 Denis Shienkov <denis.shienkov@gmail.com>
// Copyright (C) 2012 Laszlo Papp <lpapp@kde.org>
// Copyright (C) 2012 Andre Hartmann <aha_1980@gmx.de>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "JZSerialPortPrivate.h"
#include <algorithm>
#include "JZScopeGuard.h"

#ifndef CTL_CODE
#  define CTL_CODE(DeviceType, Function, Method, Access) ( \
    ((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method) \
    )
#endif

#ifndef FILE_DEVICE_SERIAL_PORT
#  define FILE_DEVICE_SERIAL_PORT  27
#endif

#ifndef METHOD_BUFFERED
#  define METHOD_BUFFERED  0
#endif

#ifndef FILE_ANY_ACCESS
#  define FILE_ANY_ACCESS  0x00000000
#endif

#ifndef IOCTL_SERIAL_GET_DTRRTS
#  define IOCTL_SERIAL_GET_DTRRTS \
    CTL_CODE(FILE_DEVICE_SERIAL_PORT, 30, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef SERIAL_DTR_STATE
#  define SERIAL_DTR_STATE  0x00000001
#endif

#ifndef SERIAL_RTS_STATE
#  define SERIAL_RTS_STATE  0x00000002
#endif

static inline void qt_set_common_props(DCB *dcb)
{
    dcb->fBinary = TRUE;
    dcb->fAbortOnError = FALSE;
    dcb->fNull = FALSE;
    dcb->fErrorChar = FALSE;

    if (dcb->fDtrControl == DTR_CONTROL_HANDSHAKE)
        dcb->fDtrControl = DTR_CONTROL_DISABLE;

    if (dcb->fRtsControl != RTS_CONTROL_HANDSHAKE)
        dcb->fRtsControl = RTS_CONTROL_DISABLE;
}

static inline void qt_set_baudrate(DCB *dcb, int32_t baudrate)
{
    dcb->BaudRate = baudrate;
}

static inline void qt_set_databits(DCB *dcb, JZSerialPort::DataBits databits)
{
    dcb->ByteSize = databits;
}

static inline void qt_set_parity(DCB *dcb, JZSerialPort::Parity parity)
{
    dcb->fParity = TRUE;
    switch (parity) {
    case JZSerialPort::NoParity:
        dcb->Parity = NOPARITY;
        dcb->fParity = FALSE;
        break;
    case JZSerialPort::OddParity:
        dcb->Parity = ODDPARITY;
        break;
    case JZSerialPort::EvenParity:
        dcb->Parity = EVENPARITY;
        break;
    case JZSerialPort::MarkParity:
        dcb->Parity = MARKPARITY;
        break;
    case JZSerialPort::SpaceParity:
        dcb->Parity = SPACEPARITY;
        break;
    default:
        dcb->Parity = NOPARITY;
        dcb->fParity = FALSE;
        break;
    }
}

static inline void qt_set_stopbits(DCB *dcb, JZSerialPort::StopBits stopbits)
{
    switch (stopbits) {
    case JZSerialPort::OneStop:
        dcb->StopBits = ONESTOPBIT;
        break;
    case JZSerialPort::OneAndHalfStop:
        dcb->StopBits = ONE5STOPBITS;
        break;
    case JZSerialPort::TwoStop:
        dcb->StopBits = TWOSTOPBITS;
        break;
    default:
        dcb->StopBits = ONESTOPBIT;
        break;
    }
}

static inline void qt_set_flowcontrol(DCB *dcb, JZSerialPort::FlowControl flowcontrol)
{
    dcb->fInX = FALSE;
    dcb->fOutX = FALSE;
    dcb->fOutxCtsFlow = FALSE;
    if (dcb->fRtsControl == RTS_CONTROL_HANDSHAKE)
        dcb->fRtsControl = RTS_CONTROL_DISABLE;
    switch (flowcontrol) {
    case JZSerialPort::NoFlowControl:
        break;
    case JZSerialPort::SoftwareControl:
        dcb->fInX = TRUE;
        dcb->fOutX = TRUE;
        break;
    case JZSerialPort::HardwareControl:
        dcb->fOutxCtsFlow = TRUE;
        dcb->fRtsControl = RTS_CONTROL_HANDSHAKE;
        break;
    default:
        break;
    }
}

bool JZSerialPortPrivate::open()
{
    DWORD desiredAccess = 0;

    handle = ::CreateFileA(systemLocation.c_str(),
                              desiredAccess, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        setError(getSystemError());
        return false;
    }

    if (initialize())
        return true;

    ::CloseHandle(handle);
    return false;
}

void JZSerialPortPrivate::close()
{
    ::CancelIo(handle);

    delete notifier;
    notifier = nullptr;

    delete startAsyncWriteTimer;
    startAsyncWriteTimer = nullptr;

    communicationStarted = false;
    readStarted = false;
    writeStarted = false;
    writeBuffer.clear();

    if (settingsRestoredOnClose) {
        ::SetCommState(handle, &restoredDcb);
        ::SetCommTimeouts(handle, &restoredCommTimeouts);
    }

    ::CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
}

bool JZSerialPortPrivate::setDataTerminalReady(bool set)
{
    if (!::EscapeCommFunction(handle, set ? SETDTR : CLRDTR)) {
        setError(getSystemError());
        return false;
    }

    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    dcb.fDtrControl = set ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    return setDcb(&dcb);
}

bool JZSerialPortPrivate::setRequestToSend(bool set)
{
    if (!::EscapeCommFunction(handle, set ? SETRTS : CLRRTS)) {
        setError(getSystemError());
        return false;
    }

    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    dcb.fRtsControl = set ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
    return setDcb(&dcb);
}

bool JZSerialPortPrivate::flush()
{
    return _q_startAsyncWrite();
}

bool JZSerialPortPrivate::clear()
{
    DWORD flags = 0;
    JZSerialPort::Direction directions = JZSerialPort::AllDirections;
    if (directions & JZSerialPort::Input)
        flags |= PURGE_RXABORT | PURGE_RXCLEAR;
    if (directions & JZSerialPort::Output)
        flags |= PURGE_TXABORT | PURGE_TXCLEAR;
    if (!::PurgeComm(handle, flags)) {
        setError(getSystemError());
        return false;
    }

    // We need start async read because a reading can be stalled. Since the
    // PurgeComm can abort of current reading sequence, or a port is in hardware
    // flow control mode, or a port has a limited read buffer size.
    if (directions & JZSerialPort::Input)
        startAsyncCommunication();

    return true;
}

bool JZSerialPortPrivate::sendBreak(int duration)
{
    if (!setBreakEnabled(true))
        return false;

    ::Sleep(duration);

    if (!setBreakEnabled(false))
        return false;

    return true;
}

bool JZSerialPortPrivate::setBreakEnabled(bool set)
{
    if (set ? !::SetCommBreak(handle) : !::ClearCommBreak(handle)) {
        setError(getSystemError());
        return false;
    }

    return true;
}

bool JZSerialPortPrivate::waitForReadyRead(int msecs)
{
    if (!writeStarted && !_q_startAsyncWrite())
        return false;

    const int64_t initialReadBufferSize = buffer.size();
    int64_t currentReadBufferSize = initialReadBufferSize;

    QDeadlineTimer deadline(msecs);

    do {
        const OVERLAPPED *overlapped = waitForNotified(deadline);
        if (!overlapped)
            return false;

        if (overlapped == &readCompletionOverlapped) {
            const int64_t readBytesForOneReadOperation = int64_t(buffer.size()) - currentReadBufferSize;
            if (readBytesForOneReadOperation == JZSerialPort_BUFFERSIZE) {
                currentReadBufferSize = buffer.size();
            } else if (readBytesForOneReadOperation == 0) {
                if (initialReadBufferSize != currentReadBufferSize)
                    return true;
            } else {
                return true;
            }
        }

    } while (!deadline.hasExpired());

    return false;
}

bool JZSerialPortPrivate::waitForBytesWritten(int msecs)
{
    if (writeBuffer.isEmpty() && writeChunkBuffer.isEmpty())
        return false;

    if (!writeStarted && !_q_startAsyncWrite())
        return false;

    QDeadlineTimer deadline(msecs);

    for (;;) {
        const OVERLAPPED *overlapped = waitForNotified(deadline);
        if (!overlapped)
            return false;

         if (overlapped == &writeCompletionOverlapped)
            return true;
    }

    return false;
}

bool JZSerialPortPrivate::setBaudRate(int32_t baudRate)
{
    auto directions = JZSerialPort::AllDirections;
    if (directions != JZSerialPort::AllDirections) {
        setError(JZSerialPortErrorInfo(JZSerialPort::UnsupportedOperationError, ("Custom baud rate direction is unsupported")));
        return false;
    }

    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    qt_set_baudrate(&dcb, baudRate);

    return setDcb(&dcb);
}

bool JZSerialPortPrivate::setDataBits(JZSerialPort::DataBits dataBits)
{
    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    qt_set_databits(&dcb, dataBits);

    return setDcb(&dcb);
}

bool JZSerialPortPrivate::setParity(JZSerialPort::Parity parity)
{
    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    qt_set_parity(&dcb, parity);

    return setDcb(&dcb);
}

bool JZSerialPortPrivate::setStopBits(JZSerialPort::StopBits stopBits)
{
    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    qt_set_stopbits(&dcb, stopBits);

    return setDcb(&dcb);
}

bool JZSerialPortPrivate::setFlowControl(JZSerialPort::FlowControl flowControl)
{
    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    qt_set_flowcontrol(&dcb, flowControl);

    return setDcb(&dcb);
}

bool JZSerialPortPrivate::completeAsyncCommunication(int64_t bytesTransferred)
{
    communicationStarted = false;

    if (bytesTransferred == int64_t(-1))
        return false;

    return startAsyncRead();
}

bool JZSerialPortPrivate::completeAsyncRead(int64_t bytesTransferred)
{
    if (bytesTransferred == int64_t(-1)) {
        readStarted = false;
        return false;
    }
    if (bytesTransferred > 0)
        buffer.append(readChunkBuffer.constData(), bytesTransferred);

    readStarted = false;

    bool result = true;
    if (bytesTransferred == JZSerialPort_BUFFERSIZE
            || queuedBytesCount(JZSerialPort::Input) > 0) {
        result = startAsyncRead();
    } else {
        result = startAsyncCommunication();
    }

    if (bytesTransferred > 0)
        emitReadyRead();

    return result;
}

bool JZSerialPortPrivate::completeAsyncWrite(int64_t bytesTransferred)
{
    if (writeStarted) {
        if (bytesTransferred == int64_t(-1)) {
            writeChunkBuffer.clear();
            writeStarted = false;
            return false;
        }
        JZ_ASSERT(bytesTransferred == writeChunkBuffer.size());
        writeChunkBuffer.clear();
        
        writeStarted = false;
    }

    return _q_startAsyncWrite();
}

bool JZSerialPortPrivate::startAsyncCommunication()
{
    if (communicationStarted)
        return true;

    ::ZeroMemory(&communicationOverlapped, sizeof(communicationOverlapped));
    if (!::WaitCommEvent(handle, &triggeredEventMask, &communicationOverlapped)) {
        JZSerialPortErrorInfo error = getSystemError();
        if (error.errorCode != JZSerialPort::NoError) {
            if (error.errorCode == JZSerialPort::PermissionError)
                error.errorCode = JZSerialPort::ResourceError;
            setError(error);
            return false;
        }
    }
    communicationStarted = true;
    return true;
}

bool JZSerialPortPrivate::startAsyncRead()
{
    if (readStarted)
        return true;

    int64_t bytesToRead = JZSerialPort_BUFFERSIZE;

    if (readBufferMaxSize && bytesToRead > (readBufferMaxSize - buffer.size())) {
        bytesToRead = readBufferMaxSize - buffer.size();
        if (bytesToRead <= 0) {
            // Buffer is full. User must read data from the buffer
            // before we can read more from the port.
            return false;
        }
    }

    JZ_ASSERT(int(bytesToRead) <= readChunkBuffer.size());

    ::ZeroMemory(&readCompletionOverlapped, sizeof(readCompletionOverlapped));
    if (::ReadFile(handle, readChunkBuffer.data(), bytesToRead, nullptr, &readCompletionOverlapped)) {
        readStarted = true;
        return true;
    }

    JZSerialPortErrorInfo error = getSystemError();
    if (error.errorCode != JZSerialPort::NoError) {
        if (error.errorCode == JZSerialPort::PermissionError)
            error.errorCode = JZSerialPort::ResourceError;
        if (error.errorCode != JZSerialPort::ResourceError)
            error.errorCode = JZSerialPort::ReadError;
        setError(error);
        return false;
    }

    readStarted = true;
    return true;
}

bool JZSerialPortPrivate::_q_startAsyncWrite()
{
    if (handle == INVALID_HANDLE_VALUE) {
        // Prevent a crash if the application incorrectly has not called open() before
        // calling this (QTBUG-120412)
        return false;
    }

    if (writeBuffer.isEmpty() || writeStarted)
        return true;

    writeChunkBuffer = writeBuffer;
    writeBuffer.clear();
    ::ZeroMemory(&writeCompletionOverlapped, sizeof(writeCompletionOverlapped));
    if (!::WriteFile(handle, writeChunkBuffer.constData(),
                     writeChunkBuffer.size(), nullptr, &writeCompletionOverlapped)) {

        JZSerialPortErrorInfo error = getSystemError();
        if (error.errorCode != JZSerialPort::NoError) {
            if (error.errorCode != JZSerialPort::ResourceError)
                error.errorCode = JZSerialPort::WriteError;
            setError(error);
            return false;
        }
    }

    writeStarted = true;
    return true;
}

void JZSerialPortPrivate::_q_notified(DWORD numberOfBytes, DWORD errorCode, OVERLAPPED *overlapped)
{
    const JZSerialPortErrorInfo error = getSystemError(errorCode);
    if (error.errorCode != JZSerialPort::NoError) {
        setError(error);
        return;
    }

    if (overlapped == &communicationOverlapped)
        completeAsyncCommunication(numberOfBytes);
    else if (overlapped == &readCompletionOverlapped)
        completeAsyncRead(numberOfBytes);
    else if (overlapped == &writeCompletionOverlapped)
        completeAsyncWrite(numberOfBytes);
    else
        JZ_ASSERT(!"Unknown OVERLAPPED activated");
}

void JZSerialPortPrivate::emitReadyRead()
{
    if (!emittedReadyRead) {
        emittedReadyRead = true;
        q->sigReadyRead.emit();
        emittedReadyRead = false;
    }
}

int64_t JZSerialPortPrivate::writeData(const char *data, int64_t maxSize)
{
    int64_t toAppend = maxSize;

    if (writeBufferMaxSize && (writeBuffer.size() + toAppend > writeBufferMaxSize))
        toAppend = writeBufferMaxSize - writeBuffer.size();

    writeBuffer.append(data, toAppend);

    if (!writeBuffer.isEmpty() && !writeStarted) {
        if (!startAsyncWriteTimer) {
            startAsyncWriteTimer = new JZTimer(q);
            connect(&startAsyncWriteTimer->sigTimeout, this, &JZSerialPortPrivate::_q_startAsyncWrite);
            startAsyncWriteTimer->setSingleShot(true);
        }
        if (!startAsyncWriteTimer->isActive())
            startAsyncWriteTimer->start(20);
    }
    return toAppend;
}

OVERLAPPED *JZSerialPortPrivate::waitForNotified(QDeadlineTimer deadline)
{
    OVERLAPPED *overlapped = notifier->waitForAnyNotified(deadline);
    if (!overlapped) {
        setError(getSystemError(WAIT_TIMEOUT));
        return nullptr;
    }
    return overlapped;
}

int64_t JZSerialPortPrivate::queuedBytesCount(JZSerialPort::Direction direction) const
{
    COMSTAT comstat;
    if (::ClearCommError(handle, nullptr, &comstat) == 0)
        return -1;
    return (direction == JZSerialPort::Input)
            ? comstat.cbInQue
            : ((direction == JZSerialPort::Output) ? comstat.cbOutQue : -1);
}

inline bool JZSerialPortPrivate::initialize()
{
    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    restoredDcb = dcb;

    if (!::GetCommTimeouts(handle, &restoredCommTimeouts)) {
        setError(getSystemError());
        return false;
    }

    JZScopeGuard restoreParametersOnError(
        [this]
        {
        if (settingsRestoredOnClose) {
            ::SetCommState(handle, &restoredDcb);
            ::SetCommTimeouts(handle, &restoredCommTimeouts);
        }
    });

    qt_set_common_props(&dcb);
    qt_set_baudrate(&dcb, inputBaudRate);
    qt_set_databits(&dcb, dataBits);
    qt_set_parity(&dcb, parity);
    qt_set_stopbits(&dcb, stopBits);
    qt_set_flowcontrol(&dcb, flowControl);

    if (!setDcb(&dcb))
        return false;

    ::ZeroMemory(&currentCommTimeouts, sizeof(currentCommTimeouts));
    currentCommTimeouts.ReadIntervalTimeout = MAXDWORD;

    if (!::SetCommTimeouts(handle, &currentCommTimeouts)) {
        setError(getSystemError());
        return false;
    }

    const DWORD eventMask = (mode & QIODevice::ReadOnly) ? EV_RXCHAR : 0;
    if (!::SetCommMask(handle, eventMask)) {
        setError(getSystemError());
        return false;
    }

    notifier = new QWinOverlappedIoNotifier(q);
    JZObjectPrivate::connect(notifier, &QWinOverlappedIoNotifier::notified,
               this, &JZSerialPortPrivate::_q_notified);
    notifier->setHandle(handle);
    notifier->setEnabled(true);

    if ((eventMask & EV_RXCHAR) && !startAsyncCommunication()) {
        delete notifier;
        notifier = nullptr;
        return false;
    }

    restoreParametersOnError.dismiss();
    return true;
}

bool JZSerialPortPrivate::setDcb(DCB *dcb)
{
    if (!::SetCommState(handle, dcb)) {
        setError(getSystemError());
        return false;
    }
    return true;
}

bool JZSerialPortPrivate::getDcb(DCB *dcb)
{
    ::ZeroMemory(dcb, sizeof(DCB));
    dcb->DCBlength = sizeof(DCB);

    if (!::GetCommState(handle, dcb)) {
        setError(getSystemError());
        return false;
    }
    return true;
}

JZSerialPortErrorInfo JZSerialPortPrivate::getSystemError(int systemErrorCode) const
{
    if (systemErrorCode == -1)
        systemErrorCode = ::GetLastError();

    JZSerialPortErrorInfo error;
    error.errorString = "systemErrorCode:" + to_string(systemErrorCode);

    switch (systemErrorCode) {
    case ERROR_SUCCESS:
        error.errorCode = JZSerialPort::NoError;
        break;
    case ERROR_IO_PENDING:
        error.errorCode = JZSerialPort::NoError;
        break;
    case ERROR_MORE_DATA:
        error.errorCode = JZSerialPort::NoError;
        break;
    case ERROR_FILE_NOT_FOUND:
        error.errorCode = JZSerialPort::DeviceNotFoundError;
        break;
    case ERROR_PATH_NOT_FOUND:
        error.errorCode = JZSerialPort::DeviceNotFoundError;
        break;
    case ERROR_INVALID_NAME:
        error.errorCode = JZSerialPort::DeviceNotFoundError;
        break;
    case ERROR_ACCESS_DENIED:
        error.errorCode = JZSerialPort::PermissionError;
        break;
    case ERROR_INVALID_HANDLE:
        error.errorCode = JZSerialPort::ResourceError;
        break;
    case ERROR_INVALID_PARAMETER:
        error.errorCode = JZSerialPort::UnsupportedOperationError;
        break;
    case ERROR_BAD_COMMAND:
        error.errorCode = JZSerialPort::ResourceError;
        break;
    case ERROR_DEVICE_REMOVED:
        error.errorCode = JZSerialPort::ResourceError;
        break;
    case ERROR_OPERATION_ABORTED:
        error.errorCode = JZSerialPort::ResourceError;
        break;
    case WAIT_TIMEOUT:
        error.errorCode = JZSerialPort::TimeoutError;
        break;
    default:
        error.errorCode = JZSerialPort::UnknownError;
        break;
    }
    return error;
}

// This table contains standard values of baud rates that
// are defined in file winbase.h
std::vector<int32_t> JZSerialPortPrivate::standardBaudRates()
{
    static const std::vector<int32_t> baudRates = {
        CBR_110,   CBR_300,   CBR_600,    CBR_1200,   CBR_2400,
        CBR_4800,  CBR_9600,  CBR_14400,  CBR_19200,  CBR_38400,
        CBR_56000, CBR_57600, CBR_115200, CBR_128000, CBR_256000
    };

    return baudRates;
}

JZSerialPort::Handle JZSerialPort::handle() const
{
    return d->handle;
}