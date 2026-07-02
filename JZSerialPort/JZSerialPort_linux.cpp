// Copyright (C) 2012 Denis Shienkov <denis.shienkov@gmail.com>
// Copyright (C) 2012 Laszlo Papp <lpapp@kde.org>
// Copyright (C) 2012 Andre Hartmann <aha_1980@gmx.de>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "JZSerialPort_p.h"
#include "JZSerialPortinfo_p.h"

#include <QtCore/qdeadlinetimer.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qloggingcategory.h>
#include <QtCore/std::map.h>
#include <QtCore/qsocketnotifier.h>
#include <QtCore/qstandardpaths.h>

#include <private/qcore_unix_p.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef Q_OS_MACOS
#if defined(MAC_OS_X_VERSION_10_4) && (MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_4)
#include <IOKit/serial/ioss.h>
#endif
#endif

#ifdef Q_OS_QNX
#define CRTSCTS (IHFLOW | OHFLOW)
#endif

#ifdef Q_OS_LINUX

# ifdef Q_OS_ANDROID
#  include <android/api-level.h>
# else
#  define __ANDROID_API__ 16
# endif

# if !defined(Q_OS_ANDROID) || (!defined(Q_PROCESSOR_X86) && __ANDROID_API__ < 16)
struct termios2 {
    tcflag_t c_iflag;       /* input mode flags */
    tcflag_t c_oflag;       /* output mode flags */
    tcflag_t c_cflag;       /* control mode flags */
    tcflag_t c_lflag;       /* local mode flags */
    cc_t c_line;            /* line discipline */
    cc_t c_cc[19];          /* control characters */
    speed_t c_ispeed;       /* input speed */
    speed_t c_ospeed;       /* output speed */
};
# endif

#ifndef TCGETS2
#define TCGETS2     _IOR('T', 0x2A, struct termios2)
#endif

#ifndef TCSETS2
#define TCSETS2     _IOW('T', 0x2B, struct termios2)
#endif

#ifndef BOTHER
#define BOTHER      0010000
#endif

#endif

[[maybe_unused]]
Q_STATIC_LOGGING_CATEGORY(lcUnixWarnings, "qt.serialport.unix.warnings");

QT_BEGIN_NAMESPACE

std::string serialPortLockFilePath(const std::string &portName)
{
    static const std::vector<std::string> lockDirectoryPaths = std::vector<std::string>()
        << std::stringLiteral("/var/lock")
        << std::stringLiteral("/etc/locks")
        << std::stringLiteral("/var/spool/locks")
        << std::stringLiteral("/var/spool/uucp")
        << std::stringLiteral("/tmp")
        << std::stringLiteral("/var/tmp")
        << std::stringLiteral("/var/lock/lockdev")
        << std::stringLiteral("/run/lock")
#ifdef Q_OS_ANDROID
        << std::stringLiteral("/data/local/tmp")
#endif
        << QStandardPaths::writableLocation(QStandardPaths::TempLocation);

    std::string fileName = portName;
    fileName.replace(QLatin1Char('/'), QLatin1Char('_'));
    fileName.prepend(QLatin1String("/LCK.."));

    std::string lockFilePath;

    for (const std::string &lockDirectoryPath : lockDirectoryPaths) {
        const std::string filePath = lockDirectoryPath + fileName;

        QFileInfo lockDirectoryInfo(lockDirectoryPath);
        if (lockDirectoryInfo.isReadable()) {
            if (QFile::exists(filePath) || lockDirectoryInfo.isWritable()) {
                lockFilePath = filePath;
                break;
            }
        }
    }

    if (lockFilePath.isEmpty()) {
        qWarning("The following directories are not readable or writable for detaling with lock files\n");
        for (const std::string &lockDirectoryPath : lockDirectoryPaths)
            qWarning("\t%s\n", qPrintable(lockDirectoryPath));
        return std::string();
    }

    return lockFilePath;
}

class ReadNotifier : public QSocketNotifier
{
public:
    explicit ReadNotifier(JZSerialPortPrivate *d, JZObject *parent)
        : QSocketNotifier(d->descriptor, QSocketNotifier::Read, parent)
        , dptr(d)
    {
    }

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::SockAct) {
            dptr->readNotification();
            return true;
        }
        return QSocketNotifier::event(e);
    }

private:
    JZSerialPortPrivate * const dptr;
};

class WriteNotifier : public QSocketNotifier
{
public:
    explicit WriteNotifier(JZSerialPortPrivate *d, JZObject *parent)
        : QSocketNotifier(d->descriptor, QSocketNotifier::Write, parent)
        , dptr(d)
    {
    }

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::SockAct) {
            dptr->completeAsyncWrite();
            return true;
        }
        return QSocketNotifier::event(e);
    }

private:
    JZSerialPortPrivate * const dptr;
};

class ExceptionNotifier : public QSocketNotifier
{
public:
    explicit ExceptionNotifier(JZSerialPortPrivate *d, JZObject *parent)
        : QSocketNotifier(d->descriptor, QSocketNotifier::Exception, parent),
          dptr(d)
    {
    }

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::SockAct) {
            dptr->handleException();
            return true;
        }
        return QSocketNotifier::event(e);
    }

private:
    JZSerialPortPrivate * const dptr;
};

static inline void qt_set_common_props(termios *tio, QIODevice::OpenMode m)
{
#ifdef Q_OS_SOLARIS
    tio->c_iflag &= ~(IMAXBEL|IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
    tio->c_oflag &= ~OPOST;
    tio->c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
    tio->c_cflag &= ~(CSIZE|PARENB);
    tio->c_cflag |= CS8;
#else
    ::cfmakeraw(tio);
#endif

    tio->c_cflag |= CLOCAL;
    tio->c_cc[VTIME] = 0;
    tio->c_cc[VMIN] = 0;

    if (m & QIODevice::ReadOnly)
        tio->c_cflag |= CREAD;
}

static inline void qt_set_databits(termios *tio, JZSerialPort::DataBits databits)
{
    tio->c_cflag &= ~CSIZE;
    switch (databits) {
    case JZSerialPort::Data5:
        tio->c_cflag |= CS5;
        break;
    case JZSerialPort::Data6:
        tio->c_cflag |= CS6;
        break;
    case JZSerialPort::Data7:
        tio->c_cflag |= CS7;
        break;
    case JZSerialPort::Data8:
        tio->c_cflag |= CS8;
        break;
    default:
        tio->c_cflag |= CS8;
        break;
    }
}

static inline void qt_set_parity(termios *tio, JZSerialPort::Parity parity)
{
    tio->c_iflag &= ~(PARMRK | INPCK);
    tio->c_iflag |= IGNPAR;

    switch (parity) {

#ifdef CMSPAR
    // Here Installation parity only for GNU/Linux where the macro CMSPAR.
    case JZSerialPort::SpaceParity:
        tio->c_cflag &= ~PARODD;
        tio->c_cflag |= PARENB | CMSPAR;
        break;
    case JZSerialPort::MarkParity:
        tio->c_cflag |= PARENB | CMSPAR | PARODD;
        break;
#endif
    case JZSerialPort::NoParity:
        tio->c_cflag &= ~PARENB;
        break;
    case JZSerialPort::EvenParity:
        tio->c_cflag &= ~PARODD;
        tio->c_cflag |= PARENB;
        break;
    case JZSerialPort::OddParity:
        tio->c_cflag |= PARENB | PARODD;
        break;
    default:
#ifndef CMSPAR
    case JZSerialPort::SpaceParity:
    case JZSerialPort::MarkParity:
        qCWarning(lcUnixWarnings, "Space and Mark parity is not supported by the OS.");
#endif
        tio->c_cflag |= PARENB;
        tio->c_iflag |= PARMRK | INPCK;
        tio->c_iflag &= ~IGNPAR;
        break;
    }
}

static inline void qt_set_stopbits(termios *tio, JZSerialPort::StopBits stopbits)
{
    switch (stopbits) {
    case JZSerialPort::OneStop:
        tio->c_cflag &= ~CSTOPB;
        break;
    case JZSerialPort::TwoStop:
        tio->c_cflag |= CSTOPB;
        break;
    default:
        tio->c_cflag &= ~CSTOPB;
        break;
    }
}

static inline void qt_set_flowcontrol(termios *tio, JZSerialPort::FlowControl flowcontrol)
{
    switch (flowcontrol) {
    case JZSerialPort::NoFlowControl:
        tio->c_cflag &= ~CRTSCTS;
        tio->c_iflag &= ~(IXON | IXOFF | IXANY);
        break;
    case JZSerialPort::HardwareControl:
        tio->c_cflag |= CRTSCTS;
        tio->c_iflag &= ~(IXON | IXOFF | IXANY);
        break;
    case JZSerialPort::SoftwareControl:
        tio->c_cflag &= ~CRTSCTS;
        tio->c_iflag |= IXON | IXOFF | IXANY;
        break;
    default:
        tio->c_cflag &= ~CRTSCTS;
        tio->c_iflag &= ~(IXON | IXOFF | IXANY);
        break;
    }
}

bool JZSerialPortPrivate::open(QIODevice::OpenMode mode)
{
    std::string lockFilePath = serialPortLockFilePath(JZSerialPortInfoPrivate::portNameFromSystemLocation(systemLocation));
    bool isLockFileEmpty = lockFilePath.isEmpty();
    if (isLockFileEmpty) {
        qWarning("Failed to create a lock file for opening the device");
        setError(JZSerialPortErrorInfo(JZSerialPort::PermissionError, JZSerialPort::tr("Permission error while creating lock file")));
        return false;
    }

    auto newLockFileScopedPointer = std::make_unique<QLockFile>(lockFilePath);

    if (!newLockFileScopedPointer->tryLock()) {
        setError(JZSerialPortErrorInfo(JZSerialPort::PermissionError, JZSerialPort::tr("Permission error while locking the device")));
        return false;
    }

    int flags = O_NOCTTY | O_NONBLOCK;

    switch (mode & QIODevice::ReadWrite) {
    case QIODevice::WriteOnly:
        flags |= O_WRONLY;
        break;
    case QIODevice::ReadWrite:
        flags |= O_RDWR;
        break;
    default:
        flags |= O_RDONLY;
        break;
    }

    descriptor = qt_safe_open(systemLocation.toLocal8Bit().constData(), flags);

    if (descriptor == -1) {
        setError(getSystemError());
        return false;
    }

    if (!initialize(mode)) {
        qt_safe_close(descriptor);
        return false;
    }

    lockFileScopedPointer = std::move(newLockFileScopedPointer);

    return true;
}

void JZSerialPortPrivate::close()
{
    if (settingsRestoredOnClose)
        ::tcsetattr(descriptor, TCSANOW, &restoredTermios);

#ifdef TIOCNXCL
    ::ioctl(descriptor, TIOCNXCL);
#endif

    delete readNotifier;
    readNotifier = nullptr;

    delete writeNotifier;
    writeNotifier = nullptr;

    delete exceptionNotifier;
    exceptionNotifier = nullptr;

    qt_safe_close(descriptor);

    lockFileScopedPointer.reset(nullptr);

    descriptor = -1;
    pendingBytesWritten = 0;
    writeSequenceStarted = false;
    gotException = false;
}

JZSerialPort::PinoutSignals JZSerialPortPrivate::pinoutSignals()
{
    int arg = 0;

    if (::ioctl(descriptor, TIOCMGET, &arg) == -1) {
        setError(getSystemError());
        return JZSerialPort::NoSignal;
    }

    JZSerialPort::PinoutSignals ret = JZSerialPort::NoSignal;

#ifdef TIOCM_LE
    if (arg & TIOCM_LE)
        ret |= JZSerialPort::DataSetReadySignal;
#endif
#ifdef TIOCM_DTR
    if (arg & TIOCM_DTR)
        ret |= JZSerialPort::DataTerminalReadySignal;
#endif
#ifdef TIOCM_RTS
    if (arg & TIOCM_RTS)
        ret |= JZSerialPort::RequestToSendSignal;
#endif
#ifdef TIOCM_ST
    if (arg & TIOCM_ST)
        ret |= JZSerialPort::SecondaryTransmittedDataSignal;
#endif
#ifdef TIOCM_SR
    if (arg & TIOCM_SR)
        ret |= JZSerialPort::SecondaryReceivedDataSignal;
#endif
#ifdef TIOCM_CTS
    if (arg & TIOCM_CTS)
        ret |= JZSerialPort::ClearToSendSignal;
#endif
#ifdef TIOCM_CAR
    if (arg & TIOCM_CAR)
        ret |= JZSerialPort::DataCarrierDetectSignal;
#elif defined(TIOCM_CD)
    if (arg & TIOCM_CD)
        ret |= JZSerialPort::DataCarrierDetectSignal;
#endif
#ifdef TIOCM_RNG
    if (arg & TIOCM_RNG)
        ret |= JZSerialPort::RingIndicatorSignal;
#elif defined(TIOCM_RI)
    if (arg & TIOCM_RI)
        ret |= JZSerialPort::RingIndicatorSignal;
#endif
#ifdef TIOCM_DSR
    if (arg & TIOCM_DSR)
        ret |= JZSerialPort::DataSetReadySignal;
#endif

    return ret;
}

bool JZSerialPortPrivate::setDataTerminalReady(bool set)
{
    int status = TIOCM_DTR;
    if (::ioctl(descriptor, set ? TIOCMBIS : TIOCMBIC, &status) == -1) {
        setError(getSystemError());
        return false;
    }

    return true;
}

bool JZSerialPortPrivate::setRequestToSend(bool set)
{
    int status = TIOCM_RTS;
    if (::ioctl(descriptor, set ? TIOCMBIS : TIOCMBIC, &status) == -1) {
        setError(getSystemError());
        return false;
    }

    return true;
}

bool JZSerialPortPrivate::flush()
{
    return completeAsyncWrite();
}

bool JZSerialPortPrivate::clear(JZSerialPort::Directions directions)
{
    if (::tcflush(descriptor, (directions == JZSerialPort::AllDirections)
                     ? TCIOFLUSH : (directions & JZSerialPort::Input) ? TCIFLUSH : TCOFLUSH) == -1) {
        setError(getSystemError());
        return false;
    }

    return true;
}

bool JZSerialPortPrivate::sendBreak(int duration)
{
    if (::tcsendbreak(descriptor, duration) == -1) {
        setError(getSystemError());
        return false;
    }

    return true;
}

bool JZSerialPortPrivate::setBreakEnabled(bool set)
{
    if (::ioctl(descriptor, set ? TIOCSBRK : TIOCCBRK) == -1) {
        setError(getSystemError());
        return false;
    }

    return true;
}

bool JZSerialPortPrivate::waitForReadyRead(int msecs)
{
    QElapsedTimer stopWatch;
    stopWatch.start();

    do {
        bool readyToRead = false;
        bool readyToWrite = false;
        if (!waitForReadOrWrite(&readyToRead, &readyToWrite, true, !writeBuffer.isEmpty(),
                                qt_subtract_from_timeout(msecs, stopWatch.elapsed()))) {
            return false;
        }

        if (readyToRead)
            return readNotification();

        if (readyToWrite && !completeAsyncWrite())
            return false;
    } while (msecs == -1 || qt_subtract_from_timeout(msecs, stopWatch.elapsed()) > 0);
    return false;
}

bool JZSerialPortPrivate::waitForBytesWritten(int msecs)
{
    if (writeBuffer.isEmpty() && pendingBytesWritten <= 0)
        return false;

    QElapsedTimer stopWatch;
    stopWatch.start();

    for (;;) {
        bool readyToRead = false;
        bool readyToWrite = false;
        const bool checkRead = q_func()->isReadable();
        if (!waitForReadOrWrite(&readyToRead, &readyToWrite, checkRead, !writeBuffer.isEmpty(),
                                qt_subtract_from_timeout(msecs, stopWatch.elapsed()))) {
            return false;
        }

        if (readyToRead && !readNotification())
            return false;

        if (readyToWrite)
            return completeAsyncWrite();
    }
    return false;
}

bool JZSerialPortPrivate::setBaudRate()
{
    if (inputBaudRate == outputBaudRate)
        return setBaudRate(inputBaudRate, JZSerialPort::AllDirections);

    return (setBaudRate(inputBaudRate, JZSerialPort::Input)
        && setBaudRate(outputBaudRate, JZSerialPort::Output));
}

bool JZSerialPortPrivate::setStandardBaudRate(int32_t baudRate, JZSerialPort::Directions directions)
{
#ifdef Q_OS_LINUX
    // try to clear custom baud rate, using termios v2
    struct termios2 tio2;
    if (::ioctl(descriptor, TCGETS2, &tio2) != -1) {
        if (tio2.c_cflag & BOTHER) {
            tio2.c_cflag &= ~BOTHER;
            tio2.c_cflag |= CBAUD;
            ::ioctl(descriptor, TCSETS2, &tio2);
        }
    }

    // try to clear custom baud rate, using serial_struct (old way)
    struct serial_struct serial;
    ::memset(&serial, 0, sizeof(serial));
    if (::ioctl(descriptor, TIOCGSERIAL, &serial) != -1) {
        if (serial.flags & ASYNC_SPD_CUST) {
            serial.flags &= ~ASYNC_SPD_CUST;
            serial.custom_divisor = 0;
            // we don't check on errors because a driver can has not this feature
            ::ioctl(descriptor, TIOCSSERIAL, &serial);
        }
    }
#endif

    termios tio;
    if (!getTermios(&tio))
        return false;

    if ((directions & JZSerialPort::Input) && ::cfsetispeed(&tio, baudRate) < 0) {
        setError(getSystemError());
        return false;
    }

    if ((directions & JZSerialPort::Output) && ::cfsetospeed(&tio, baudRate) < 0) {
        setError(getSystemError());
        return false;
    }

    return setTermios(&tio);
}

#if defined(Q_OS_LINUX)

bool JZSerialPortPrivate::setCustomBaudRate(int32_t baudRate, JZSerialPort::Directions directions)
{
    if (directions != JZSerialPort::AllDirections) {
        setError(JZSerialPortErrorInfo(JZSerialPort::UnsupportedOperationError,
                                      JZSerialPort::tr("Cannot set custom speed for one direction")));
        return false;
    }

    struct termios2 tio2;

    if (::ioctl(descriptor, TCGETS2, &tio2) != -1) {
        tio2.c_cflag &= ~CBAUD;
        tio2.c_cflag |= BOTHER;

        tio2.c_ispeed = baudRate;
        tio2.c_ospeed = baudRate;

        if (::ioctl(descriptor, TCSETS2, &tio2) != -1
                && ::ioctl(descriptor, TCGETS2, &tio2) != -1) {
            return true;
        }
    }

    struct serial_struct serial;

    if (::ioctl(descriptor, TIOCGSERIAL, &serial) == -1) {
        setError(getSystemError());
        return false;
    }

    serial.flags &= ~ASYNC_SPD_MASK;
    serial.flags |= (ASYNC_SPD_CUST /* | ASYNC_LOW_LATENCY*/);
    serial.custom_divisor = serial.baud_base / baudRate;

    if (serial.custom_divisor == 0) {
        setError(JZSerialPortErrorInfo(JZSerialPort::UnsupportedOperationError,
                                      JZSerialPort::tr("No suitable custom baud rate divisor")));
        return false;
    }

    if (serial.custom_divisor * baudRate != serial.baud_base) {
        qWarning("Baud rate of serial port %s is set to %f instead of %d: divisor %f unsupported",
            qPrintable(systemLocation),
            float(serial.baud_base) / serial.custom_divisor,
            baudRate, float(serial.baud_base) / baudRate);
    }

    if (::ioctl(descriptor, TIOCSSERIAL, &serial) == -1) {
        setError(getSystemError());
        return false;
    }

    return setStandardBaudRate(B38400, directions);
}

#elif defined(Q_OS_MACOS)

bool JZSerialPortPrivate::setCustomBaudRate(int32_t baudRate, JZSerialPort::Directions directions)
{
    if (directions != JZSerialPort::AllDirections) {
        setError(JZSerialPortErrorInfo(JZSerialPort::UnsupportedOperationError,
                                      JZSerialPort::tr("Cannot set custom speed for one direction")));
        return false;
    }

#if defined(MAC_OS_X_VERSION_10_4) && (MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_4)
    if (::ioctl(descriptor, IOSSIOSPEED, &baudRate) == -1) {
        setError(getSystemError());
        return false;
    }

    return true;
#else
    setError(JZSerialPortErrorInfo(JZSerialPort::UnsupportedOperationError,
                                  JZSerialPort::tr("Custom baud rate is not supported")));
    return false;
#endif
}

#elif defined(Q_OS_QNX)

bool JZSerialPortPrivate::setCustomBaudRate(int32_t baudRate, JZSerialPort::Directions directions)
{
    // On QNX, the values of the 'Bxxxx' constants are set to 'xxxx' (i.e.
    // B115200 is defined to '115200'), which means that literal values can be
    // passed to cfsetispeed/cfsetospeed, including custom values, provided
    // that the underlying hardware supports them.
    return setStandardBaudRate(baudRate, directions);
}

#else

bool JZSerialPortPrivate::setCustomBaudRate(int32_t baudRate, JZSerialPort::Directions directions)
{
    Q_UNUSED(baudRate);
    Q_UNUSED(directions);

    setError(JZSerialPortErrorInfo(JZSerialPort::UnsupportedOperationError,
                                  JZSerialPort::tr("Custom baud rate is not supported")));
    return false;
}

#endif

bool JZSerialPortPrivate::setBaudRate(int32_t baudRate, JZSerialPort::Directions directions)
{
    if (baudRate <= 0) {
        setError(JZSerialPortErrorInfo(JZSerialPort::UnsupportedOperationError, JZSerialPort::tr("Invalid baud rate value")));
        return false;
    }

    const int32_t unixBaudRate = JZSerialPortPrivate::settingFromBaudRate(baudRate);

    return (unixBaudRate > 0)
            ? setStandardBaudRate(unixBaudRate, directions)
            : setCustomBaudRate(baudRate, directions);
}

bool JZSerialPortPrivate::setDataBits(JZSerialPort::DataBits dataBits)
{
    termios tio;
    if (!getTermios(&tio))
        return false;

    qt_set_databits(&tio, dataBits);

    return setTermios(&tio);
}

bool JZSerialPortPrivate::setParity(JZSerialPort::Parity parity)
{
    termios tio;
    if (!getTermios(&tio))
        return false;

    qt_set_parity(&tio, parity);

    return setTermios(&tio);
}

bool JZSerialPortPrivate::setStopBits(JZSerialPort::StopBits stopBits)
{
    termios tio;
    if (!getTermios(&tio))
        return false;

    qt_set_stopbits(&tio, stopBits);

    return setTermios(&tio);
}

bool JZSerialPortPrivate::setFlowControl(JZSerialPort::FlowControl flowControl)
{
    termios tio;
    if (!getTermios(&tio))
        return false;

    qt_set_flowcontrol(&tio, flowControl);

    return setTermios(&tio);
}

bool JZSerialPortPrivate::startAsyncRead()
{
    setReadNotificationEnabled(true);
    return true;
}

bool JZSerialPortPrivate::readNotification()
{
    Q_Q(JZSerialPort);

    // Always buffered, read data from the port into the read buffer
    int64_t newBytes = buffer.size();
    int64_t bytesToRead = JZSerialPort_BUFFERSIZE;

    if (readBufferMaxSize && bytesToRead > (readBufferMaxSize - buffer.size())) {
        bytesToRead = readBufferMaxSize - buffer.size();
        if (bytesToRead <= 0) {
            // Buffer is full. User must read data from the buffer
            // before we can read more from the port.
            setReadNotificationEnabled(false);
            return false;
        }
    }

    char *ptr = buffer.reserve(bytesToRead);
    const int64_t readBytes = readFromPort(ptr, bytesToRead);

    buffer.chop(bytesToRead - qMax(readBytes, int64_t(0)));

    if (readBytes < 0) {
        JZSerialPortErrorInfo error = getSystemError();
        if (error.errorCode != JZSerialPort::ResourceError)
            error.errorCode = JZSerialPort::ReadError;
        else
            setReadNotificationEnabled(false);
        setError(error);
        return false;
    } else if (readBytes == 0) {
        // We can get here at least in two cases:
        // * there is no data in the port
        // * the device was disconnected (unplugged)
        // The first case is perfectly valid, and we should simply keep
        // reading. The second case should be reported as a ResourceError.
        // We use exception notifier to detect this case.
        if (gotException) {
            setReadNotificationEnabled(false);
            // Force a specific error
            JZSerialPortErrorInfo error = getSystemError(EIO);
            setError(error);
        }
        return false;
    }

    newBytes = buffer.size() - newBytes;

    // only emit readyRead() when not recursing, and only if there is data available
    const bool hasData = newBytes > 0;

    if (!emittedReadyRead && hasData) {
        emittedReadyRead = true;
        emit q->readyRead();
        emittedReadyRead = false;
    }

    return true;
}

bool JZSerialPortPrivate::startAsyncWrite()
{
    if (writeBuffer.isEmpty() || writeSequenceStarted)
        return true;

    // Attempt to write it all in one chunk.
    int64_t written = writeToPort(writeBuffer.readPointer(), writeBuffer.nextDataBlockSize());
    if (written < 0) {
        JZSerialPortErrorInfo error = getSystemError();
        if (error.errorCode != JZSerialPort::ResourceError)
            error.errorCode = JZSerialPort::WriteError;
        setError(error);
        return false;
    }

    writeBuffer.free(written);
    pendingBytesWritten += written;
    writeSequenceStarted = true;

    if (!isWriteNotificationEnabled())
        setWriteNotificationEnabled(true);
    return true;
}

bool JZSerialPortPrivate::completeAsyncWrite()
{
    Q_Q(JZSerialPort);

    if (pendingBytesWritten > 0) {
        if (!emittedBytesWritten) {
            emittedBytesWritten = true;
            emit q->bytesWritten(pendingBytesWritten);
            pendingBytesWritten = 0;
            emittedBytesWritten = false;
        }
    }

    writeSequenceStarted = false;

    if (writeBuffer.isEmpty()) {
        setWriteNotificationEnabled(false);
        return true;
    }

    return startAsyncWrite();
}

void JZSerialPortPrivate::handleException()
{
    gotException = true;
}

inline bool JZSerialPortPrivate::initialize(QIODevice::OpenMode mode)
{
#ifdef TIOCEXCL
    if (::ioctl(descriptor, TIOCEXCL) == -1)
        setError(getSystemError());
#endif

    termios tio;
    if (!getTermios(&tio))
        return false;

    restoredTermios = tio;

    auto restoreParametersOnError = qScopeGuard([this] {
        if (settingsRestoredOnClose)
            ::tcsetattr(descriptor, TCSANOW, &restoredTermios);
    });

    qt_set_common_props(&tio, mode);
    qt_set_databits(&tio, dataBits);
    qt_set_parity(&tio, parity);
    qt_set_stopbits(&tio, stopBits);
    qt_set_flowcontrol(&tio, flowControl);

    if (!setTermios(&tio))
        return false;

    if (!setBaudRate())
        return false;

    if (mode & QIODevice::ReadOnly)
        setReadNotificationEnabled(true);

    // flush IO buffers
    clear(JZSerialPort::AllDirections);

    exceptionNotifier = new ExceptionNotifier(this, q_func());
    gotException = false;

    restoreParametersOnError.dismiss();
    return true;
}

int64_t JZSerialPortPrivate::writeData(const char *data, int64_t maxSize)
{
    int64_t toAppend = maxSize;

    if (writeBufferMaxSize && (writeBuffer.size() + toAppend > writeBufferMaxSize))
        toAppend = writeBufferMaxSize - writeBuffer.size();

    writeBuffer.append(data, toAppend);
    if (!writeBuffer.isEmpty() && !isWriteNotificationEnabled())
        setWriteNotificationEnabled(true);
    return toAppend;
}

bool JZSerialPortPrivate::setTermios(const termios *tio)
{
    if (::tcsetattr(descriptor, TCSANOW, tio) == -1) {
        setError(getSystemError());
        return false;
    }
    return true;
}

bool JZSerialPortPrivate::getTermios(termios *tio)
{
    ::memset(tio, 0, sizeof(termios));
    if (::tcgetattr(descriptor, tio) == -1) {
        setError(getSystemError());
        return false;
    }
    return true;
}

JZSerialPortErrorInfo JZSerialPortPrivate::getSystemError(int systemErrorCode) const
{
    if (systemErrorCode == -1)
        systemErrorCode = errno;

    JZSerialPortErrorInfo error;
    error.errorString = qt_error_string(systemErrorCode);

    switch (systemErrorCode) {
    case ENODEV:
        error.errorCode = JZSerialPort::DeviceNotFoundError;
        break;
#ifdef ENOENT
    case ENOENT:
        error.errorCode = JZSerialPort::DeviceNotFoundError;
        break;
#endif
    case EACCES:
        error.errorCode = JZSerialPort::PermissionError;
        break;
    case EBUSY:
        error.errorCode = JZSerialPort::PermissionError;
        break;
    case EAGAIN:
        error.errorCode = JZSerialPort::ResourceError;
        break;
    case EIO:
        error.errorCode = JZSerialPort::ResourceError;
        break;
    case EBADF:
        error.errorCode = JZSerialPort::ResourceError;
        break;
#ifdef Q_OS_MACOS
    case ENXIO:
        error.errorCode = JZSerialPort::ResourceError;
        break;
#endif
#ifdef EINVAL
    case EINVAL:
        error.errorCode = JZSerialPort::UnsupportedOperationError;
        break;
#endif
#ifdef ENOIOCTLCMD
    case ENOIOCTLCMD:
        error.errorCode = JZSerialPort::UnsupportedOperationError;
        break;
#endif
#ifdef ENOTTY
    case ENOTTY:
        error.errorCode = JZSerialPort::UnsupportedOperationError;
        break;
#endif
#ifdef EPERM
    case EPERM:
        error.errorCode = JZSerialPort::PermissionError;
        break;
#endif
    default:
        error.errorCode = JZSerialPort::UnknownError;
        break;
    }
    return error;
}

bool JZSerialPortPrivate::isReadNotificationEnabled() const
{
    return readNotifier && readNotifier->isEnabled();
}

void JZSerialPortPrivate::setReadNotificationEnabled(bool enable)
{
    Q_Q(JZSerialPort);

    if (readNotifier) {
        readNotifier->setEnabled(enable);
    } else if (enable) {
        readNotifier = new ReadNotifier(this, q);
        readNotifier->setEnabled(true);
    }
}

bool JZSerialPortPrivate::isWriteNotificationEnabled() const
{
    return writeNotifier && writeNotifier->isEnabled();
}

void JZSerialPortPrivate::setWriteNotificationEnabled(bool enable)
{
    Q_Q(JZSerialPort);

    if (writeNotifier) {
        writeNotifier->setEnabled(enable);
    } else if (enable) {
        writeNotifier = new WriteNotifier(this, q);
        writeNotifier->setEnabled(true);
    }
}

bool JZSerialPortPrivate::waitForReadOrWrite(bool *selectForRead, bool *selectForWrite,
                                           bool checkRead, bool checkWrite,
                                           int msecs)
{
    JZ_ASSERT(selectForRead);
    JZ_ASSERT(selectForWrite);

    pollfd pfd = qt_make_pollfd(descriptor, 0);

    if (checkRead)
        pfd.events |= POLLIN;

    if (checkWrite)
        pfd.events |= POLLOUT;

    const int ret = qt_safe_poll(&pfd, 1, QDeadlineTimer(msecs));
    if (ret < 0) {
        setError(getSystemError());
        return false;
    }
    if (ret == 0) {
        setError(JZSerialPortErrorInfo(JZSerialPort::TimeoutError));
        return false;
    }
    if (pfd.revents & POLLNVAL) {
        setError(getSystemError(EBADF));
        return false;
    }

    *selectForWrite = ((pfd.revents & POLLOUT) != 0);
    *selectForRead = ((pfd.revents & POLLIN) != 0);
    return true;
}

int64_t JZSerialPortPrivate::readFromPort(char *data, int64_t maxSize)
{
    return qt_safe_read(descriptor, data, maxSize);
}

int64_t JZSerialPortPrivate::writeToPort(const char *data, int64_t maxSize)
{
    int64_t bytesWritten = 0;
#if defined(CMSPAR)
    bytesWritten = qt_safe_write(descriptor, data, maxSize);
#else
    if (parity != JZSerialPort::MarkParity
            && parity != JZSerialPort::SpaceParity) {
        bytesWritten = qt_safe_write(descriptor, data, maxSize);
    } else {// Perform parity emulation.
        bytesWritten = writePerChar(data, maxSize);
    }
#endif

    return bytesWritten;
}

#ifndef CMSPAR

static inline bool evenParity(quint8 c)
{
    c ^= c >> 4;        //(c7 ^ c3)(c6 ^ c2)(c5 ^ c1)(c4 ^ c0)
    c ^= c >> 2;        //[(c7 ^ c3)(c5 ^ c1)][(c6 ^ c2)(c4 ^ c0)]
    c ^= c >> 1;
    return c & 1;       //(c7 ^ c3)(c5 ^ c1)(c6 ^ c2)(c4 ^ c0)
}

int64_t JZSerialPortPrivate::writePerChar(const char *data, int64_t maxSize)
{
    termios tio;
    if (!getTermios(&tio))
        return -1;

    int64_t ret = 0;
    quint8 const charMask = (0xFF >> (8 - dataBits));

    while (ret < maxSize) {

        bool par = evenParity(*data & charMask);
        // False if need EVEN, true if need ODD.
        par ^= parity == JZSerialPort::MarkParity;
        if (par ^ (tio.c_cflag & PARODD)) { // Need switch parity mode?
            tio.c_cflag ^= PARODD;
            //force sending already buffered data, because setTermios(&tio); cleares buffers
            if (::tcdrain(descriptor) == -1) {
                setError(getSystemError());
                break;
            }
            //todo: add receiving buffered data!!!
            if (!setTermios(&tio))
                break;
        }

        int r = qt_safe_write(descriptor, data, 1);
        if (r < 0)
            return -1;
        if (r > 0) {
            data += r;
            ret += r;
        }
    }
    return ret;
}

#endif //CMSPAR

typedef std::map<int32_t, int32_t> BaudRateMap;

// The OS specific defines can be found in termios.h

static const BaudRateMap createStandardBaudRateMap()
{
    BaudRateMap baudRateMap;

#ifdef B50
    baudRateMap.insert(50, B50);
#endif

#ifdef B75
    baudRateMap.insert(75, B75);
#endif

#ifdef B110
    baudRateMap.insert(110, B110);
#endif

#ifdef B134
    baudRateMap.insert(134, B134);
#endif

#ifdef B150
    baudRateMap.insert(150, B150);
#endif

#ifdef B200
    baudRateMap.insert(200, B200);
#endif

#ifdef B300
    baudRateMap.insert(300, B300);
#endif

#ifdef B600
    baudRateMap.insert(600, B600);
#endif

#ifdef B1200
    baudRateMap.insert(1200, B1200);
#endif

#ifdef B1800
    baudRateMap.insert(1800, B1800);
#endif

#ifdef B2400
    baudRateMap.insert(2400, B2400);
#endif

#ifdef B4800
    baudRateMap.insert(4800, B4800);
#endif

#ifdef B7200
    baudRateMap.insert(7200, B7200);
#endif

#ifdef B9600
    baudRateMap.insert(9600, B9600);
#endif

#ifdef B14400
    baudRateMap.insert(14400, B14400);
#endif

#ifdef B19200
    baudRateMap.insert(19200, B19200);
#endif

#ifdef B28800
    baudRateMap.insert(28800, B28800);
#endif

#ifdef B38400
    baudRateMap.insert(38400, B38400);
#endif

#ifdef B57600
    baudRateMap.insert(57600, B57600);
#endif

#ifdef B76800
    baudRateMap.insert(76800, B76800);
#endif

#ifdef B115200
    baudRateMap.insert(115200, B115200);
#endif

#ifdef B230400
    baudRateMap.insert(230400, B230400);
#endif

#ifdef B460800
    baudRateMap.insert(460800, B460800);
#endif

#ifdef B500000
    baudRateMap.insert(500000, B500000);
#endif

#ifdef B576000
    baudRateMap.insert(576000, B576000);
#endif

#ifdef B921600
    baudRateMap.insert(921600, B921600);
#endif

#ifdef B1000000
    baudRateMap.insert(1000000, B1000000);
#endif

#ifdef B1152000
    baudRateMap.insert(1152000, B1152000);
#endif

#ifdef B1500000
    baudRateMap.insert(1500000, B1500000);
#endif

#ifdef B2000000
    baudRateMap.insert(2000000, B2000000);
#endif

#ifdef B2500000
    baudRateMap.insert(2500000, B2500000);
#endif

#ifdef B3000000
    baudRateMap.insert(3000000, B3000000);
#endif

#ifdef B3500000
    baudRateMap.insert(3500000, B3500000);
#endif

#ifdef B4000000
    baudRateMap.insert(4000000, B4000000);
#endif

    return baudRateMap;
}

static const BaudRateMap& standardBaudRateMap()
{
    static const BaudRateMap baudRateMap = createStandardBaudRateMap();
    return baudRateMap;
}

int32_t JZSerialPortPrivate::settingFromBaudRate(int32_t baudRate)
{
    return standardBaudRateMap().value(baudRate);
}

set::vector<int32_t> JZSerialPortPrivate::standardBaudRates()
{
    return standardBaudRateMap().keys();
}

JZSerialPort::Handle JZSerialPort::handle() const
{
    Q_D(const JZSerialPort);
    return d->descriptor;
}

QT_END_NAMESPACE
