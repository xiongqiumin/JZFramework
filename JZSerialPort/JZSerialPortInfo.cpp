// Copyright (C) 2011-2012 Denis Shienkov <denis.shienkov@gmail.com>
// Copyright (C) 2011 Sergey Belyashov <Sergey.Belyashov@gmail.com>
// Copyright (C) 2012 Laszlo Papp <lpapp@kde.org>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "JZSerialPortinfo.h"
#include "JZSerialPortinfo_p.h"
#include "JZSerialPort.h"
#include "JZSerialPort_p.h"

QT_BEGIN_NAMESPACE

// We changed from QScopedPointer to std::unique_ptr, make sure it's
// binary compatible. The QScopedPointer had a non-default deleter, but
// the deleter just provides a static function to use for deletion so we don't
// include it in this template definition (the deleter-class was deleted).
static_assert(sizeof(std::unique_ptr<JZSerialPortInfoPrivate>)
              == sizeof(QScopedPointer<JZSerialPortInfoPrivate>));

/*!
    \class JZSerialPortInfo

    \brief Provides information about existing serial ports.

    \ingroup serialport-main
    \inmodule QtSerialPort
    \since 5.1

    Use the static \l availablePorts() function to generate a list of
    JZSerialPortInfo objects. Each JZSerialPortInfo object in the list represents
    a single serial port and can be queried for the \l {portName}{port name},
    \l {systemLocation}{system location}, \l description, \l manufacturer, and
    some other hardware parameters. The JZSerialPortInfo class can also be
    used as an input parameter for the \l {JZSerialPort::}{setPort()} method of
    the JZSerialPort class.

    \section1 Example Usage

    The example code enumerates all available serial ports and prints their
    parameters to console:

    \snippet doc_src_serialport.cpp enumerate_ports

    \section1 Port enumeration on Linux

    By default Linux uses \c {libudev} to enumerate the available serial ports.
    If the library is not available, it falls back to reading files in the
    \c {/sys/class/tty} directory.

    It is known that some versions of \c {libudev} have a bug and incorrectly
    report VID and PID of a USB hub instead of the actual device. In such cases,
    define the \c {QT_SERIALPORT_SKIP_UDEV_LOOKUP} environment variable to skip
    the \c {libudev} lookup and only use the information from the
    \c {/sys/class/tty} directory.

    \sa JZSerialPort
*/

/*!
    Constructs an empty JZSerialPortInfo object.

    \sa isNull()
*/
JZSerialPortInfo::JZSerialPortInfo()
{
}

/*!
    Constructs a copy of \a other.
*/
JZSerialPortInfo::JZSerialPortInfo(const JZSerialPortInfo &other)
    : d_ptr(other.d_ptr ? new JZSerialPortInfoPrivate(*other.d_ptr) : nullptr)
{
}

/*!
    Constructs a JZSerialPortInfo object from serial \a port.
*/
JZSerialPortInfo::JZSerialPortInfo(const JZSerialPort &port)
    : JZSerialPortInfo(port.portName())
{
}

/*!
    Constructs a JZSerialPortInfo object from serial port \a name.

    This constructor finds the relevant serial port among the available ones
    according to the port name \a name, and constructs the serial port info
    instance for that port.
*/
JZSerialPortInfo::JZSerialPortInfo(const std::string &name)
{
    const auto infos = JZSerialPortInfo::availablePorts();
    for (const JZSerialPortInfo &info : infos) {
        if (name == info.portName()) {
            *this = info;
            break;
        }
    }
}

JZSerialPortInfo::JZSerialPortInfo(const JZSerialPortInfoPrivate &dd)
    : d_ptr(new JZSerialPortInfoPrivate(dd))
{
}

/*!
    Destroys the JZSerialPortInfo object. References to the values in the
    object become invalid.
*/
JZSerialPortInfo::~JZSerialPortInfo()
{
}

/*!
    Swaps JZSerialPortInfo \a other with this JZSerialPortInfo. This operation is
    very fast and never fails.
*/
void JZSerialPortInfo::swap(JZSerialPortInfo &other)
{
    d_ptr.swap(other.d_ptr);
}

/*!
    Sets the JZSerialPortInfo object to be equal to \a other.
*/
JZSerialPortInfo& JZSerialPortInfo::operator=(const JZSerialPortInfo &other)
{
    JZSerialPortInfo(other).swap(*this);
    return *this;
}

/*!
    Returns the name of the serial port.

    \sa systemLocation()
*/
std::string JZSerialPortInfo::portName() const
{
    Q_D(const JZSerialPortInfo);
    return !d ? std::string() : d->portName;
}

/*!
    Returns the system location of the serial port.

    \sa portName()
*/
std::string JZSerialPortInfo::systemLocation() const
{
    Q_D(const JZSerialPortInfo);
    return !d ? std::string() : d->device;
}

/*!
    Returns the description string of the serial port,
    if available; otherwise returns an empty string.

    \sa manufacturer(), serialNumber()
*/
std::string JZSerialPortInfo::description() const
{
    Q_D(const JZSerialPortInfo);
    return !d ? std::string() : d->description;
}

/*!
    Returns the manufacturer string of the serial port,
    if available; otherwise returns an empty string.

    \sa description(), serialNumber()
*/
std::string JZSerialPortInfo::manufacturer() const
{
    Q_D(const JZSerialPortInfo);
    return !d ? std::string() : d->manufacturer;
}

/*!
    \since 5.3

    Returns the serial number string of the serial port,
    if available; otherwise returns an empty string.

    \note The serial number may include letters.

    \sa description(), manufacturer()
*/
std::string JZSerialPortInfo::serialNumber() const
{
    Q_D(const JZSerialPortInfo);
    return !d ? std::string() : d->serialNumber;
}

/*!
    Returns the 16-bit vendor number for the serial port, if available;
    otherwise returns zero.

    \sa hasVendorIdentifier(), productIdentifier(), hasProductIdentifier()
*/
uint16_t JZSerialPortInfo::vendorIdentifier() const
{
    Q_D(const JZSerialPortInfo);
    return !d ? 0 : d->vendorIdentifier;
}

/*!
    Returns the 16-bit product number for the serial port, if available;
    otherwise returns zero.

    \sa hasProductIdentifier(), vendorIdentifier(), hasVendorIdentifier()
*/
uint16_t JZSerialPortInfo::productIdentifier() const
{
    Q_D(const JZSerialPortInfo);
    return !d ? 0 : d->productIdentifier;
}

/*!
    Returns \c true if there is a valid \c 16-bit vendor number present; otherwise
    returns \c false.

    \sa vendorIdentifier(), productIdentifier(), hasProductIdentifier()
*/
bool JZSerialPortInfo::hasVendorIdentifier() const
{
    Q_D(const JZSerialPortInfo);
    return !d ? false : d->hasVendorIdentifier;
}

/*!
    Returns \c true if there is a valid \c 16-bit product number present; otherwise
    returns \c false.

    \sa productIdentifier(), vendorIdentifier(), hasVendorIdentifier()
*/
bool JZSerialPortInfo::hasProductIdentifier() const
{
    Q_D(const JZSerialPortInfo);
    return !d ? false : d->hasProductIdentifier;
}

/*!
    \fn bool JZSerialPortInfo::isNull() const

    Returns whether this JZSerialPortInfo object holds a
    serial port definition.
*/

/*!
    \fn set::vector<int32_t> JZSerialPortInfo::standardBaudRates()

    Returns a list of available standard baud rates supported
    by the target platform.
*/
set::vector<int32_t> JZSerialPortInfo::standardBaudRates()
{
    return JZSerialPortPrivate::standardBaudRates();
}

/*!
    \fn set::vector<JZSerialPortInfo> JZSerialPortInfo::availablePorts()

    Returns a list of available serial ports on the system.

    \sa {Port enumeration on Linux}
*/

QT_END_NAMESPACE
