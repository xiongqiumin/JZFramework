// Copyright (C) 2012 Denis Shienkov <denis.shienkov@gmail.com>
// Copyright (C) 2012 Laszlo Papp <lpapp@kde.org>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef JZSerialPortInfo_H
#define JZSerialPortInfo_H

#include <vector>
#include <memory>
#include "JZSerialPortGlobal.h"

class JZSerialPort;
class JZSerialPortInfoPrivate;
class JZSerialPortInfo
{
public:
    JZSerialPortInfo();
    explicit JZSerialPortInfo(const JZSerialPort &port);
    explicit JZSerialPortInfo(const std::string &name);
    ~JZSerialPortInfo();

    JZSerialPortInfo& operator=(const JZSerialPortInfo &other);
    void swap(JZSerialPortInfo &other);

    std::string portName() const;
    std::string systemLocation() const;
    std::string description() const;
    std::string manufacturer() const;
    std::string serialNumber() const;

    int16_t vendorIdentifier() const;
    int16_t productIdentifier() const;
    
    bool hasVendorIdentifier() const;
    bool hasProductIdentifier() const;

    bool isNull() const { return !d_ptr; }

    static std::vector<int32_t> standardBaudRates();
    static std::vector<JZSerialPortInfo> availablePorts();

private:
    JZSerialPortInfo(const JZSerialPortInfoPrivate &dd);
    friend std::vector<JZSerialPortInfo> availablePortsByUdev(bool &ok);
    friend std::vector<JZSerialPortInfo> availablePortsBySysfs(bool &ok);
    friend std::vector<JZSerialPortInfo> availablePortsByFiltersOfDevices(bool &ok);
    std::shared_ptr<JZSerialPortInfoPrivate> d_ptr;
};

#endif // JZSerialPortInfo_H
