// Copyright (C) 2011-2012 Denis Shienkov <denis.shienkov@gmail.com>
// Copyright (C) 2011 Sergey Belyashov <Sergey.Belyashov@gmail.com>
// Copyright (C) 2012 Laszlo Papp <lpapp@kde.org>
// Copyright (C) 2012 Andre Hartmann <aha_1980@gmx.de>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include JZSerialPort
#include "JZSerialPortInfo.h"
#include "JZSerialPortInfoPrivate.h"

#include "JZSerialPortPrivate.h"

#include "JZDebug.h"

JZSerialPortErrorInfo::JZSerialPortErrorInfo(JZSerialPort::SerialPortError newErrorCode,
                                           const std::string &newErrorString)
    : errorCode(newErrorCode)
    , errorString(newErrorString)
{
    if (errorString.empty()) {
        switch (errorCode) {
        case JZSerialPort::NoError:
            errorString = ("No error");
            break;
        case JZSerialPort::OpenError:
            errorString = ("Device is already open");
            break;
        case JZSerialPort::NotOpenError:
            errorString = ("Device is not open");
            break;
        case JZSerialPort::TimeoutError:
            errorString = ("Operation timed out");
            break;
        case JZSerialPort::ReadError:
            errorString = ("Error reading from device");
            break;
        case JZSerialPort::WriteError:
            errorString = ("Error writing to device");
            break;
        case JZSerialPort::ResourceError:
            errorString = ("Device disappeared from the system");
            break;
        default:
            // an empty string will be interpreted as "Unknown error"
            // from the QIODevice::errorString()
            break;
        }
    }
}

JZSerialPortPrivate::JZSerialPortPrivate()
{
}

void JZSerialPortPrivate::setError(const JZSerialPortErrorInfo &errorInfo)
{
}

/*!
    \class JZSerialPort

    \brief Provides functions to access serial ports.

    \reentrant
    \ingroup serialport-main
    \inmodule QtSerialPort
    \since 5.1

    You can get information about the available serial ports using the
    JZSerialPortInfo helper class, which allows an enumeration of all the serial
    ports in the system. This is useful to obtain the correct name of the
    serial port you want to use. You can pass an object
    of the helper class as an argument to the setPort() or setPortName()
    methods to assign the desired serial device.

    After setting the port, you can open it in read-only (r/o), write-only
    (w/o), or read-write (r/w) mode using the open() method.

    \note The serial port is always opened with exclusive access
    (that is, no other process or thread can access an already opened serial port).

    Use the close() method to close the port and cancel the I/O operations.

    Having successfully opened, JZSerialPort tries to determine the current
    configuration of the port and initializes itself. You can reconfigure the
    port to the desired setting using the setBaudRate(), setDataBits(),
    setParity(), setStopBits(), and setFlowControl() methods.

    There are a couple of properties to work with the pinout signals namely:
    JZSerialPort::dataTerminalReady, JZSerialPort::requestToSend. It is also
    possible to use the pinoutSignals() method to query the current pinout
    signals set.

    Once you know that the ports are ready to read or write, you can
    use the read() or write() methods. Alternatively the
    readLine() and readAll() convenience methods can also be invoked.
    If not all the data is read at once, the remaining data will
    be available for later as new incoming data is appended to the
    JZSerialPort's internal read buffer. You can limit the size of the read
    buffer using setReadBufferSize().

    JZSerialPort provides a set of functions that suspend the
    calling thread until certain signals are emitted. These functions
    can be used to implement blocking serial ports:

    \list

    \li waitForReadyRead() blocks calls until new data is available for
    reading.

    \li waitForBytesWritten() blocks calls until one payload of data has
    been written to the serial port.

    \endlist

    See the following example:

    \code
     int64_t numReadTotal = 0;
     char buffer[50];

     for (;;) {
         const int64_t numRead  = serial.read(buffer, 50);

         // Do whatever with the array

         numReadTotal += numRead;
         if (numRead == 0 && !serial.waitForReadyRead())
             break;
     }
    \endcode

    If \l{QIODevice::}{waitForReadyRead()} returns \c false, the
    connection has been closed or an error has occurred.

    If an error occurs at any point in time, JZSerialPort will emit the
    errorOccurred() signal. You can also call error() to find the type of
    error that occurred last.

    Programming with a blocking serial port is radically different from
    programming with a non-blocking serial port. A blocking serial port
    does not require an event loop and typically leads to simpler code.
    However, in a GUI application, blocking serial port should only be
    used in non-GUI threads, to avoid freezing the user interface.

    For more details about these approaches, refer to the
    \l {Qt Serial Port Examples}{example} applications.

    The JZSerialPort class can also be used with QTextStream and JZDataStream's
    stream operators (operator<<() and operator>>()). There is one issue to be
    aware of, though: make sure that enough data is available before attempting
    to read by using the operator>>() overloaded operator.

    \sa JZSerialPortInfo
*/

/*!
    \enum JZSerialPort::Direction

    This enum describes the possible directions of the data transmission.

    \note This enumeration is used for setting the baud rate of the device
    separately for each direction on some operating systems (for example,
    POSIX-like).

    \value Input            Input direction.
    \value Output           Output direction.
    \value AllDirections    Simultaneously in two directions.
*/

/*!
    \enum JZSerialPort::BaudRate

    This enum describes the baud rate which the communication device operates
    with.

    \note Only the most common standard baud rates are listed in this enum.

    \value Baud1200     1200 baud.
    \value Baud2400     2400 baud.
    \value Baud4800     4800 baud.
    \value Baud9600     9600 baud.
    \value Baud19200    19200 baud.
    \value Baud38400    38400 baud.
    \value Baud57600    57600 baud.
    \value Baud115200   115200 baud.

    \sa JZSerialPort::baudRate
*/

/*!
    \enum JZSerialPort::DataBits

    This enum describes the number of data bits used.

    \value Data5            The number of data bits in each character is 5. It
                            is used for Baudot code. It generally only makes
                            sense with older equipment such as teleprinters.
    \value Data6            The number of data bits in each character is 6. It
                            is rarely used.
    \value Data7            The number of data bits in each character is 7. It
                            is used for true ASCII. It generally only makes
                            sense with older equipment such as teleprinters.
    \value Data8            The number of data bits in each character is 8. It
                            is used for most kinds of data, as this size matches
                            the size of a byte. It is almost universally used in
                            newer applications.

    \sa JZSerialPort::dataBits
*/

/*!
    \enum JZSerialPort::Parity

    This enum describes the parity scheme used.

    \value NoParity         No parity bit it sent. This is the most common
                            parity setting. Error detection is handled by the
                            communication protocol.
    \value EvenParity       The number of 1 bits in each character, including
                            the parity bit, is always even.
    \value OddParity        The number of 1 bits in each character, including
                            the parity bit, is always odd. It ensures that at
                            least one state transition occurs in each character.
    \value SpaceParity      Space parity. The parity bit is sent in the space
                            signal condition. It does not provide error
                            detection information.
    \value MarkParity       Mark parity. The parity bit is always set to the
                            mark signal condition (logical 1). It does not
                            provide error detection information.

    \sa JZSerialPort::parity
*/

/*!
    \enum JZSerialPort::StopBits

    This enum describes the number of stop bits used.

    \value OneStop          1 stop bit.
    \value OneAndHalfStop   1.5 stop bits. This is only for the Windows platform.
    \value TwoStop          2 stop bits.

    \sa JZSerialPort::stopBits
*/

/*!
    \enum JZSerialPort::FlowControl

    This enum describes the flow control used.

    \value NoFlowControl        No flow control.
    \value HardwareControl      Hardware flow control (RTS/CTS).
    \value SoftwareControl      Software flow control (XON/XOFF).

    \sa JZSerialPort::flowControl
*/

/*!
    \enum JZSerialPort::PinoutSignal

    This enum describes the possible RS-232 pinout signals.

    \value NoSignal                       No line active
    \value DataTerminalReadySignal        DTR (Data Terminal Ready).
    \value DataCarrierDetectSignal        DCD (Data Carrier Detect).
    \value DataSetReadySignal             DSR (Data Set Ready).
    \value RingIndicatorSignal            RNG (Ring Indicator).
    \value RequestToSendSignal            RTS (Request To Send).
    \value ClearToSendSignal              CTS (Clear To Send).
    \value SecondaryTransmittedDataSignal STD (Secondary Transmitted Data).
    \value SecondaryReceivedDataSignal    SRD (Secondary Received Data).

    \sa pinoutSignals(), JZSerialPort::dataTerminalReady,
    JZSerialPort::requestToSend
*/

/*!
    \enum JZSerialPort::SerialPortError

    This enum describes the errors that may be contained by the
    JZSerialPort::error property.

    \value NoError              No error occurred.

    \value DeviceNotFoundError  An error occurred while attempting to
                                open an non-existing device.

    \value PermissionError      An error occurred while attempting to
                                open an already opened device by another
                                process or a user not having enough permission
                                and credentials to open.

    \value OpenError            An error occurred while attempting to open an
                                already opened device in this object.

    \value NotOpenError         This error occurs when an operation is executed
                                that can only be successfully performed if the
                                device is open. This value was introduced in
                                QtSerialPort 5.2.

    \value WriteError           An I/O error occurred while writing the data.

    \value ReadError            An I/O error occurred while reading the data.

    \value ResourceError        An I/O error occurred when a resource becomes
                                unavailable, e.g. when the device is
                                unexpectedly removed from the system.

    \value UnsupportedOperationError The requested device operation is not
                                supported or prohibited by the running operating
                                system.

    \value TimeoutError         A timeout error occurred. This value was
                                introduced in QtSerialPort 5.2.

    \value UnknownError         An unidentified error occurred.
    \sa JZSerialPort::error
*/



/*!
    Constructs a new serial port object with the given \a parent.
*/
JZSerialPort::JZSerialPort(JZObject *parent) 
    :JZObject(parent), sigReadyRead(this)
{
    d = new JZSerialPortPrivate();
}

/*!
    Constructs a new serial port object with the given \a parent
    to represent the serial port with the specified \a name.

    The name should have a specific format; see the setPort() method.
*/
JZSerialPort::JZSerialPort(const std::string &name, JZObject *parent) 
    :JZObject(parent), sigReadyRead(this)
{
    setPortName(name);
}

/*!
    Constructs a new serial port object with the given \a parent
    to represent the serial port with the specified helper class
    \a serialPortInfo.
*/
JZSerialPort::JZSerialPort(const JZSerialPortInfo &serialPortInfo, JZObject *parent)
    : JZObject(parent), sigReadyRead(this)
{
    setPort(serialPortInfo);
}

/*!
    Closes the serial port, if necessary, and then destroys object.
*/
JZSerialPort::~JZSerialPort()
{
    /**/
    if (isOpen())
        close();
    
    delete d;
}

/*!
    Sets the \a name of the serial port.

    The name of the serial port can be passed as either a short name or
    the long system location if necessary.

    \sa portName(), JZSerialPortInfo
*/
void JZSerialPort::setPortName(const std::string &name)
{
    d->systemLocation = JZSerialPortInfoPrivate::portNameToSystemLocation(name);
}

/*!
    Sets the port stored in the serial port info instance \a serialPortInfo.

    \sa portName(), JZSerialPortInfo
*/
void JZSerialPort::setPort(const JZSerialPortInfo &serialPortInfo)
{
    d->systemLocation = serialPortInfo.systemLocation();
}

/*!
    Returns the name set by setPort() or passed to the JZSerialPort constructor.
    This name is short, i.e. it is extracted and converted from the internal
    variable system location of the device. The conversion algorithm is
    platform specific:
    \table
    \header
        \li Platform
        \li Brief Description
    \row
        \li Windows
        \li Removes the prefix "\\\\.\\" or "//./" from the system location
           and returns the remainder of the string.
    \row
        \li Unix, BSD
        \li Removes the prefix "/dev/" from the system location
           and returns the remainder of the string.
    \endtable

    \sa setPort(), JZSerialPortInfo::portName()
*/
std::string JZSerialPort::portName() const
{
    return JZSerialPortInfoPrivate::portNameFromSystemLocation(d->systemLocation);
}

/*!
    \reimp

    Opens the serial port using OpenMode \a mode, and then returns \c true if
    successful; otherwise returns \c false and sets an error code which can be
    obtained by calling the error() method.

    If the port is opened, but setting the desired port parameters fails, the
    method returns \c false and closes the port automatically.

    \warning The \a mode has to be QIODeviceBase::ReadOnly, QIODeviceBase::WriteOnly,
    or QIODeviceBase::ReadWrite. Other modes are unsupported.

    \note Due to historical reasons, upon a successful open the
    \l errorOccurred() signal is emitted with the \l {JZSerialPort::}{NoError}
    error code. This behavior is preserved to keep backwards compatibility.

    \sa QIODeviceBase::OpenMode, setPort()
*/
bool JZSerialPort::open()
{
    if (isOpen()) {
        d->setError(JZSerialPortErrorInfo(JZSerialPort::OpenError));
        return false;
    }
    
    clearError();
    if (!d->open())
        return false;

    return true;
}

/*!
    \reimp

    \note The serial port has to be open before trying to close it; otherwise
    sets the NotOpenError error code.

    \sa QIODevice::close()
*/
void JZSerialPort::close()
{
    if (!isOpen()) {
        d->setError(JZSerialPortErrorInfo(JZSerialPort::NotOpenError));
        return;
    }

    d->close();
}

/*!
    \property JZSerialPort::baudRate
    \brief the data baud rate for the desired direction

    If the setting is successful or set before opening the port, returns \c true;
    otherwise returns \c false and sets an error code which can be obtained by
    accessing the value of the JZSerialPort::error property. To set the baud
    rate, use the enumeration JZSerialPort::BaudRate or any positive int32_t
    value.

    \note If the setting is set before opening the port, the actual serial port
    setting is done automatically in the \l{JZSerialPort::open()} method right
    after that the opening of the port succeeds.

    \warning Setting the AllDirections flag is supported on all platforms.
    Windows supports only this mode.

    \warning Returns equal baud rate in any direction on Windows.

    The default value is Baud9600, i.e. 9600 bits per second.
*/
bool JZSerialPort::setBaudRate(int32_t baudRate)
{
    if (!isOpen() || d->setBaudRate(baudRate)) {
        d->inputBaudRate = baudRate;
        d->outputBaudRate = baudRate;
        return true;
    }

    return false;
}

int32_t JZSerialPort::baudRate() const
{
    return d->inputBaudRate;
}

/*!
    \fn void JZSerialPort::baudRateChanged(int32_t baudRate, Directions directions)

    This signal is emitted after the baud rate has been changed. The new baud
    rate is passed as \a baudRate and directions as \a directions.

    \sa JZSerialPort::baudRate
*/

/*!
    \property JZSerialPort::dataBits
    \brief the data bits in a frame

    If the setting is successful or set before opening the port, returns
    \c true; otherwise returns \c false and sets an error code which can be obtained
    by accessing the value of the JZSerialPort::error property.

    \note If the setting is set before opening the port, the actual serial port
    setting is done automatically in the \l{JZSerialPort::open()} method right
    after that the opening of the port succeeds.

    The default value is Data8, i.e. 8 data bits.
*/
bool JZSerialPort::setDataBits(DataBits dataBits)
{
    const auto currentDataBits = d->dataBits;
    if (!isOpen() || d->setDataBits(dataBits)) {
        if (currentDataBits != dataBits) {
            d->dataBits = dataBits;
        }
        return true;
    }
    return false;
}

JZSerialPort::DataBits JZSerialPort::dataBits() const
{
    return d->dataBits;
}

/*!
    \fn void JZSerialPort::dataBitsChanged(DataBits dataBits)

    This signal is emitted after the data bits in a frame has been changed. The
    new data bits in a frame is passed as \a dataBits.

    \sa JZSerialPort::dataBits
*/


/*!
    \property JZSerialPort::parity
    \brief the parity checking mode

    If the setting is successful or set before opening the port, returns \c true;
    otherwise returns \c false and sets an error code which can be obtained by
    accessing the value of the JZSerialPort::error property.

    \note If the setting is set before opening the port, the actual serial port
    setting is done automatically in the \l{JZSerialPort::open()} method right
    after that the opening of the port succeeds.

    The default value is NoParity, i.e. no parity.

    \warning Some UNIX operating systems (i.e. \macOS) do not support
    \l {https://www.man7.org/linux/man-pages/man3/termios.3.html#DESCRIPTION}
    {the CMSPAR flag}. On such systems, setting \l {JZSerialPort::Parity}
    {Mark or Space} parity is not supported.
*/
bool JZSerialPort::setParity(Parity parity)
{
    const auto currentParity = d->parity;
    if (!isOpen() || d->setParity(parity)) {
        if (currentParity != parity) {
            d->parity = parity;
        }
        return true;
    }
    return false;
}

JZSerialPort::Parity JZSerialPort::parity() const
{
    return d->parity;
}

/*!
    \fn void JZSerialPort::parityChanged(Parity parity)

    This signal is emitted after the parity checking mode has been changed. The
    new parity checking mode is passed as \a parity.

    \sa JZSerialPort::parity
*/

/*!
    \property JZSerialPort::stopBits
    \brief the number of stop bits in a frame

    If the setting is successful or set before opening the port, returns \c true;
    otherwise returns \c false and sets an error code which can be obtained by
    accessing the value of the JZSerialPort::error property.

    \note If the setting is set before opening the port, the actual serial port
    setting is done automatically in the \l{JZSerialPort::open()} method right
    after that the opening of the port succeeds.

    The default value is OneStop, i.e. 1 stop bit.
*/
bool JZSerialPort::setStopBits(StopBits stopBits)
{
    const auto currentStopBits = d->stopBits;
    if (!isOpen() || d->setStopBits(stopBits)) {
        if (currentStopBits != stopBits) {
            d->stopBits = stopBits;
        }
        return true;
    }
    return false;
}

JZSerialPort::StopBits JZSerialPort::stopBits() const
{
    return d->stopBits;
}

/*!
    \fn void JZSerialPort::stopBitsChanged(StopBits stopBits)

    This signal is emitted after the number of stop bits in a frame has been
    changed. The new number of stop bits in a frame is passed as \a stopBits.

    \sa JZSerialPort::stopBits
*/

/*!
    \property JZSerialPort::flowControl
    \brief the desired flow control mode

    If the setting is successful or set before opening the port, returns \c true;
    otherwise returns \c false and sets an error code which can be obtained by
    accessing the value of the JZSerialPort::error property.

    \note If the setting is set before opening the port, the actual serial port
    setting is done automatically in the \l{JZSerialPort::open()} method right
    after that the opening of the port succeeds.

    The default value is NoFlowControl, i.e. no flow control.
*/
bool JZSerialPort::setFlowControl(FlowControl flowControl)
{
    const auto currentFlowControl = d->flowControl;
    if (!isOpen() || d->setFlowControl(flowControl)) {
        if (currentFlowControl != flowControl) {
            d->flowControl = flowControl;
        }
        return true;
    }
    return false;
}

JZSerialPort::FlowControl JZSerialPort::flowControl() const
{
    return d->flowControl;
}

/*!
    \fn void JZSerialPort::flowControlChanged(FlowControl flow)

    This signal is emitted after the flow control mode has been changed. The
    new flow control mode is passed as \a flow.

    \sa JZSerialPort::flowControl
*/

/*!
    \property JZSerialPort::dataTerminalReady
    \brief the state (high or low) of the line signal DTR

    Returns \c true on success, \c false otherwise.
    If the flag is \c true then the DTR signal is set to high; otherwise low.

    \note The serial port has to be open before trying to set or get this
    property; otherwise \c false is returned and the error code is set to
    NotOpenError.

    \sa pinoutSignals()
*/
bool JZSerialPort::setDataTerminalReady(bool set)
{
    return false;
}

bool JZSerialPort::isDataTerminalReady()
{
    return false;
}

/*!
    \fn void JZSerialPort::dataTerminalReadyChanged(bool set)

    This signal is emitted after the state (high or low) of the line signal DTR
    has been changed. The new the state (high or low) of the line signal DTR is
    passed as \a set.

    \sa JZSerialPort::dataTerminalReady
*/

/*!
    \property JZSerialPort::requestToSend
    \brief the state (high or low) of the line signal RTS

    Returns \c true on success, \c false otherwise.
    If the flag is \c true then the RTS signal is set to high; otherwise low.

    \note The serial port has to be open before trying to set or get this
    property; otherwise \c false is returned and the error code is set to
    NotOpenError.

    \note An attempt to control the RTS signal in the HardwareControl mode
    will fail with error code set to UnsupportedOperationError, because
    the signal is automatically controlled by the driver.

    \sa pinoutSignals()
*/
bool JZSerialPort::setRequestToSend(bool set)
{
    if (!isOpen()) {
        d->setError(JZSerialPortErrorInfo(JZSerialPort::NotOpenError));
        jzWarning("%s: device not open", JZ_FUNC_INFO);
        return false;
    }

    if (d->flowControl == JZSerialPort::HardwareControl) {
        d->setError(JZSerialPortErrorInfo(JZSerialPort::UnsupportedOperationError));
        return false;
    }

    const bool requestToSend = isRequestToSend();
    const bool retval = d->setRequestToSend(set);
    if (retval && (requestToSend != set))
    {
        
    }

    return retval;
}

bool JZSerialPort::isRequestToSend()
{
    return false;
}

/*!
    This function writes as much as possible from the internal write
    buffer to the underlying serial port without blocking. If any data
    was written, this function returns \c true; otherwise returns \c false.

    Call this function for sending the buffered data immediately to the serial
    port. The number of bytes successfully written depends on the operating
    system. In most cases, this function does not need to be called, because the
    JZSerialPort class will start sending data automatically once control is
    returned to the event loop. In the absence of an event loop, call
    waitForBytesWritten() instead.

    \note The serial port has to be open before trying to flush any buffered
    data; otherwise returns \c false and sets the NotOpenError error code.

    \sa write(), waitForBytesWritten()
*/
bool JZSerialPort::flush()
{
    if (!isOpen()) {
        d->setError(JZSerialPortErrorInfo(JZSerialPort::NotOpenError));
        jzWarning("%s: device not open", JZ_FUNC_INFO);
        return false;
    }

    return d->flush();
}

/*!
    Discards all characters from the output or input buffer, depending on
    given directions \a directions. This includes clearing the internal class buffers and
    the UART (driver) buffers. Also terminate pending read or write operations.
    If successful, returns \c true; otherwise returns \c false.

    \note The serial port has to be open before trying to clear any buffered
    data; otherwise returns \c false and sets the NotOpenError error code.
*/
void JZSerialPort::clear()
{
    if (!isOpen()) {
        d->setError(JZSerialPortErrorInfo(JZSerialPort::NotOpenError));
        jzWarning("%s: device not open", JZ_FUNC_INFO);
        return;
    }

    d->readBuffer.clear();
    d->writeBuffer.clear();
    d->clear();
}

/*!
    \property JZSerialPort::error
    \brief the error status of the serial port

    The I/O device status returns an error code. For example, if open()
    returns \c false, or a read/write operation returns \c -1, this property can
    be used to figure out the reason why the operation failed.

    The error code is set to the default JZSerialPort::NoError after a call to
    clearError()
*/
JZSerialPort::SerialPortError JZSerialPort::error() const
{
    return d->error;
}

void JZSerialPort::clearError()
{
    d->setError(JZSerialPortErrorInfo(JZSerialPort::NoError));
}

/*!
    \fn void JZSerialPort::errorOccurred(SerialPortError error)
    \since 5.8

    This signal is emitted when an error occurs in the serial port.
    The specified \a error describes the type of error that occurred.

    \sa JZSerialPort::error
*/

/*!
    Returns the size of the internal read buffer. This limits the
    amount of data that the client can receive before calling the read()
    or readAll() methods.

    A read buffer size of \c 0 (the default) means that the buffer has
    no size limit, ensuring that no data is lost.

    \sa setReadBufferSize(), read()
*/
int64_t JZSerialPort::readBufferSize() const
{
    return d->readBufferMaxSize;
}

/*!
    Sets the size of JZSerialPort's internal read buffer to be \a
    size bytes.

    If the buffer size is limited to a certain size, JZSerialPort
    will not buffer more than this size of data. The special case of a buffer
    size of \c 0 means that the read buffer is unlimited and all
    incoming data is buffered. This is the default.

    This option is useful if the data is only read at certain points
    in time (for instance in a real-time streaming application) or if the serial
    port should be protected against receiving too much data, which may
    eventually cause the application to run out of memory.

    \sa readBufferSize(), read()
*/
void JZSerialPort::setReadBufferSize(int64_t size)
{
    d->readBufferMaxSize = size;
    if (isReadable())
        d->startAsyncRead();
}

/*!
    \since 6.10

    Returns the size of the internal write buffer.

    A write buffer size of \c 0 (the default) means that the buffer has
    no size limit.

    \sa setWriteBufferSize(), write()
*/
int64_t JZSerialPort::writeBufferSize() const
{
    return d->writeBufferMaxSize;
}

/*!
    \since 6.10

    Sets the size of JZSerialPort's internal write buffer to be \a
    size bytes.

    Sending the data over serial port is relatively slow, so in practice, when
    \l write() is called, the data is not sent immediately. It is first stored
    in an intermediate buffer and later written in chunks.

    Thus, an attempt to write too much data or write data at a higher rate than
    the underlying serial port can handle, can lead to a situation when the
    internal buffer will grow. This can eventually cause the application to
    run out of memory, specially on a device with low memory resources.

    This method allows to limit the internal buffer to a certain size. If the
    next write attempt exceeds the capacity of the buffer, the \l write()
    method will return the amount of bytes that were actually stored in the
    buffer. It's the user's responsibility to repeat the write attempt with the
    rest of the bytes after the \l bytesWritten() signal was received, or after
    the \l waitForBytesWritten() method returns \c true.

    Passing \c 0 to this method means that the input buffer is not limited, and
    all the incoming data is buffered. This is the default.

    \sa writeBufferSize(), write()
*/
void JZSerialPort::setWriteBufferSize(int64_t size)
{
    if (size < 0)
        size = 0;
    d->writeBufferMaxSize = size;
    if (isWritable())
        d->flush();
}

/*!
    \reimp

    Always returns \c true. The serial port is a sequential device.
*/
bool JZSerialPort::isSequential() const
{
    return true;
}

/*!
    \reimp

    Returns the number of incoming bytes that are waiting to be read.

    \sa bytesToWrite(), read()
*/
int64_t JZSerialPort::bytesAvailable() const
{
    return 0;
}

/*!
    \reimp

    Returns the number of bytes that are waiting to be written. The
    bytes are written when control goes back to the event loop or
    when flush() is called.

    \sa bytesAvailable(), flush()
*/
int64_t JZSerialPort::bytesToWrite() const
{
    int64_t pendingBytes = 0;
#if defined(WIN32)
    pendingBytes += d->writeChunkBuffer.size();
#endif
    return pendingBytes;
}

/*!
    \reimp

    Returns \c true if a line of data can be read from the serial port;
    otherwise returns \c false.

    \sa readLine()
*/
bool JZSerialPort::canReadLine() const
{
    JZ_ASSERT(0);
    return false;
}

/*!
    \reimp

    This function blocks until new data is available for reading and the
    \l{QIODevice::}{readyRead()} signal has been emitted. The function
    will timeout after \a msecs milliseconds; the default timeout is
    30000 milliseconds. If \a msecs is -1, this function will not time out.

    The function returns \c true if the readyRead() signal is emitted and
    there is new data available for reading; otherwise it returns \c false
    (if an error occurred or the operation timed out).

    \sa waitForBytesWritten()
*/
bool JZSerialPort::waitForReadyRead(int msecs)
{
    return d->waitForReadyRead(msecs);
}

/*!
    \fn Handle JZSerialPort::handle() const
    \since 5.2

    If the platform is supported and the serial port is open, returns the native
    serial port handle; otherwise returns \c -1.

    \warning This function is for expert use only; use it at your own risk.
    Furthermore, this function carries no compatibility promise between minor
    Qt releases.
*/

/*!
    \reimp

    This function blocks until at least one byte has been written to the serial
    port and the \l{QIODevice::}{bytesWritten()} signal has been emitted. The
    function will timeout after \a msecs milliseconds; the default timeout is
    30000 milliseconds. If \a msecs is -1, this function will not time out.

    The function returns \c true if the bytesWritten() signal is emitted; otherwise
    it returns \c false (if an error occurred or the operation timed out).
*/
bool JZSerialPort::waitForBytesWritten(int msecs)
{
    return d->waitForBytesWritten(msecs);
}

/*!
    \property JZSerialPort::breakEnabled
    \since 5.5
    \brief the state of the transmission line in break

    Returns \c true on success, \c false otherwise.
    If the flag is \c true then the transmission line is in break state;
    otherwise is in non-break state.

    \note The serial port has to be open before trying to set or get this
    property; otherwise returns \c false and sets the NotOpenError error code.
    This is a bit unusual as opposed to the regular Qt property settings of
    a class. However, this is a special use case since the property is set
    through the interaction with the kernel and hardware. Hence, the two
    scenarios cannot be completely compared to each other.
*/
bool JZSerialPort::setBreakEnabled(bool set)
{
    return false;
}

bool JZSerialPort::isBreakEnabled() const
{
    return false;
}

/*!
    \fn void JZSerialPort::settingsRestoredOnCloseChanged(bool restore)
    \since 6.9

    This signal is emitted after the settingsRestoredOnClose property is
    changed. The \a restore parameter contains the new value of the property.

    \sa JZSerialPort::settingsRestoredOnClose
*/

/*!
    \property JZSerialPort::settingsRestoredOnClose
    \since 6.9
    \brief This property defines if the port parameters should be restored on
    close or not

    After the port is opened, the class caches its parameters before applying
    the parameters that are defined by the user.

    If the property is \c true, the serial port tries to restore the cached
    parameters before closing the port; otherwise the caches parameters are
    discarded.

    The default value is \c true.

    \note This property may have no effect on some operating systems.
    For example, \macOS seems to always restore the default serial port settings
    when the port is closed.
*/
bool JZSerialPort::settingsRestoredOnClose() const
{
    return d->settingsRestoredOnClose;
}

void JZSerialPort::setSettingsRestoredOnClose(bool restore)
{
    if (d->settingsRestoredOnClose == restore)
        return;

    d->settingsRestoredOnClose = restore;
}

/*!
    \reimp

    \omit
    This function does not really read anything, as we use QIODevicePrivate's
    buffer. The buffer will be read inside of QIODevice before this
    method will be called.
    \endomit
*/
int64_t JZSerialPort::readData(char *data, int64_t maxSize)
{
    // In any case we need to start the notifications if they were
    // disabled by the read handler. If enabled, next call does nothing.
    d->startAsyncRead();

    // return 0 indicating there may be more data in the future
    return int64_t(0);
}

/*!
    \reimp
*/
int64_t JZSerialPort::readLineData(char *data, int64_t maxSize)
{
    JZ_ASSERT(0);
    return 0; //QIODevice::readLineData(data, maxSize);
}

/*!
    \reimp
*/
int64_t JZSerialPort::writeData(const char *data, int64_t maxSize)
{
    return d->writeData(data, maxSize);
}