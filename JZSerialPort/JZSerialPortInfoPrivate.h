// Copyright (C) 2011-2012 Denis Shienkov <denis.shienkov@gmail.com>
// Copyright (C) 2017 Sergey Belyashov <Sergey.Belyashov@gmail.com>
// Copyright (C) 2013 Laszlo Papp <lpapp@kde.org>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef JZSerialPortINFO_P_H
#define JZSerialPortINFO_P_H

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

class JZSerialPortInfoPrivate
{
public:
    static std::string portNameToSystemLocation(const std::string &source);
    static std::string portNameFromSystemLocation(const std::string &source);

    std::string portName;
    std::string device;
    std::string description;
    std::string manufacturer;
    std::string serialNumber;

    uint16_t vendorIdentifier = 0;
    uint16_t productIdentifier = 0;

    bool hasVendorIdentifier = false;
    bool hasProductIdentifier = false;
};

#endif // JZSerialPortINFO_P_H
