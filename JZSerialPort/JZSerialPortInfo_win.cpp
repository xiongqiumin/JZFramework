// Copyright (C) 2011-2012 Denis Shienkov <denis.shienkov@gmail.com>
// Copyright (C) 2011 Sergey Belyashov <Sergey.Belyashov@gmail.com>
// Copyright (C) 2012 Laszlo Papp <lpapp@kde.org>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:critical reason:data-parser

#include "JZSerialPortInfo.h"
#include "JZSerialPortInfoPrivate.h"
#include "JZSerialPortPrivate.h"
#include "JZStd.h"
#include "JZScopeGuard.h"
#include "JZUuid.h"

#include <vector>
#include <array>

#include <initguid.h>
#include <devguid.h> // for GUID_DEVCLASS_PORTS and GUID_DEVCLASS_MODEM
#include <winioctl.h> // for GUID_DEVINTERFACE_COMPORT
#include <setupapi.h>
#include <cfgmgr32.h>
#include <usbioctl.h>
#include <usbiodef.h>
#include <ioapiset.h>

#ifdef QT_NO_REDEFINE_GUID_DEVINTERFACE_MODEM
#  include <ntddmodm.h> // for GUID_DEVINTERFACE_MODEM
#else
  DEFINE_GUID(GUID_DEVINTERFACE_MODEM, 0x2c7089aa, 0x2e0e, 0x11d1, 0xb1, 0x14, 0x00, 0xc0, 0x4f, 0xc2, 0xaa, 0xe4);
#endif

namespace {

struct DevInfoHandle
{
    static HDEVINFO invalidValue() noexcept
    {
        return INVALID_HANDLE_VALUE;
    }

    explicit DevInfoHandle(HDEVINFO h)
    {
        m_handle = h;
    }

    ~DevInfoHandle()
    {
        close();
    }

    explicit operator bool() const noexcept
    {
        return m_handle != invalidValue();
    }
        
    HDEVINFO get() const
    {
        return m_handle;
    }
    
    bool close() noexcept
    {
        if (m_handle == INVALID_HANDLE_VALUE)
            return false;

        auto handle = m_handle;
        m_handle = INVALID_HANDLE_VALUE;
        return SetupDiDestroyDeviceInfoList(handle) == TRUE;
    }

protected:
    JZ_DISABLE_COPY(DevInfoHandle)

    HDEVINFO m_handle;
};

} // namespace

static std::vector<std::string> portNamesFromHardwareDeviceMap()
{
    std::vector<std::string> result;

    HKEY key = nullptr;
    LSTATUS openRet = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_QUERY_VALUE, &key);
    if (openRet != ERROR_SUCCESS)
        return result;
    
    DWORD index = 0;

    // This is a maximum length of value name, see:
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms724872%28v=vs.85%29.aspx
    enum { MaximumValueNameInChars = 16383 };

    std::vector<char> outputValueName(MaximumValueNameInChars, 0);
    std::vector<char> outputBuffer(MAX_PATH + 1, 0);
    DWORD bytesRequired = MAX_PATH;
    for (;;) {
        DWORD requiredValueNameChars = MaximumValueNameInChars;
        const LONG ret = ::RegEnumValue(key, index, &outputValueName[0], &requiredValueNameChars,
                                        nullptr, nullptr, reinterpret_cast<PBYTE>(&outputBuffer[0]), &bytesRequired);
        if (ret == ERROR_MORE_DATA) {
            outputBuffer.resize(bytesRequired / sizeof(char) + 2, 0);
        } else if (ret == ERROR_SUCCESS) {
            result.push_back(&outputBuffer[0]);
            ++index;
        } else {
            break;
        }
    }
    return result;
}

static std::string deviceRegistryProperty(HDEVINFO deviceInfoSet,
                                      PSP_DEVINFO_DATA deviceInfoData,
                                      DWORD property)
{
    DWORD dataType = 0;
    std::vector<char> outputBuffer(MAX_PATH + 1, 0);
    DWORD bytesRequired = MAX_PATH;
    for (;;) {
        if (::SetupDiGetDeviceRegistryProperty(deviceInfoSet, deviceInfoData, property, &dataType,
                                               reinterpret_cast<PBYTE>(&outputBuffer[0]),
                                               bytesRequired, &bytesRequired)) {
            break;
        }

        if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER
                || (dataType != REG_SZ && dataType != REG_EXPAND_SZ)) {
            return std::string();
        }
        outputBuffer.resize(bytesRequired / sizeof(char) + 2, 0);
    }
    return &outputBuffer[0];
}

static std::string getStringDescriptor(HANDLE hHubDevice, ULONG connectionIndex,
                                   UCHAR descriptorIndex, USHORT languageID)
{

    constexpr DWORD bufferSize = sizeof(USB_DESCRIPTOR_REQUEST) + MAXIMUM_USB_STRING_LENGTH;
    std::array<UCHAR, bufferSize> buffer{};
    auto request = reinterpret_cast<PUSB_DESCRIPTOR_REQUEST>(buffer.data());

    request->ConnectionIndex = connectionIndex;
    request->SetupPacket.wValue = (USB_STRING_DESCRIPTOR_TYPE << 8) | descriptorIndex;
    request->SetupPacket.wIndex = languageID;
    request->SetupPacket.wLength = MAXIMUM_USB_STRING_LENGTH;

    ULONG   bytesReturned = 0;
    BOOL success = DeviceIoControl(hHubDevice, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
                                   request, bufferSize, request, bufferSize, &bytesReturned,
                                   nullptr);

    if (!success)
        return {};

    constexpr ULONG minExpectedSize = sizeof(USB_DESCRIPTOR_REQUEST) +
            sizeof(USB_STRING_DESCRIPTOR);

    if (bytesReturned < minExpectedSize)
        return {};

    const auto stringDesc = reinterpret_cast<PUSB_STRING_DESCRIPTOR>(request->Data);

    if (stringDesc->bDescriptorType != USB_STRING_DESCRIPTOR_TYPE)
        return {};

    if (stringDesc->bLength != bytesReturned - sizeof(USB_DESCRIPTOR_REQUEST))
        return {};

    if (stringDesc->bLength % 2 != 0)
        return {};

    // Offset per USB 2.0 standard, section 9.6.7 table 9-16.
    // bLength appears to include the size of the first 2 bytes of the descriptor
    constexpr int stringOffset = 2;
    const int numWideChars = (stringDesc->bLength - stringOffset) / sizeof(char);
    return wstr_to_str(std::wstring(stringDesc->bString, numWideChars));
}

static std::vector<char> getDevicePath(DEVINST devInst)
{
    unsigned long devIdSize;

    if (::CM_Get_Device_ID_Size(&devIdSize, devInst, 0) != CR_SUCCESS) {
        return {};
    }

    std::vector<char> buffer(devIdSize + 1);

    if (::CM_Get_Device_IDA(devInst, buffer.data(), devIdSize, 0) != CR_SUCCESS) {
        return {};
    }

    return buffer;
}

// Returns true if a hub is found, false if we gave up and didn't find one
// Passes devLocation by ref - port number on the hub of the device we need info for.
// Passes buffer by ref - path to the device we need to query to get information
static bool getUSBLocationAndPath(DWORD devInst, int &devLocation, std::vector<char> &buffer)
{
    bool hubFound = false;
    int count = 0;
    std::string devLocationStr;
    std::vector<char> devicePath = {};


    // In order to find the actual information we want to find in subsequent function calls,
    // We have to get the port number on the hub of the device we want from that device,
    // and we have to get the path to the hub device. Both are required to get
    // any of the usb data we want. So we loop through starting with the passed-in
    // device instance, and we return (via out arguments) the port of the device connected
    // to the hub, and the path to the hub itself.

    while (!hubFound && (count < 3)) {
        DEVINST parentDevInst;
        if (count == 0) {
            parentDevInst = devInst;
        } else {
            if (::CM_Get_Parent(&parentDevInst, devInst, 0) != CR_SUCCESS) {
                return false;
            }
        }

        devicePath = getDevicePath(parentDevInst);

        if (devicePath.empty())
            return false;

        // Get info for this device. If it's a composite device we'll have to go up another level.

        const DevInfoHandle currDeviceInformation{ ::SetupDiGetClassDevsA(
                nullptr, devicePath.data(), nullptr,
                DIGCF_ALLCLASSES | DIGCF_PRESENT | DIGCF_DEVICEINTERFACE) };

        if (!currDeviceInformation)
            return false;

        SP_DEVINFO_DATA currDeviceInfoData = {};
        currDeviceInfoData.cbSize = sizeof(currDeviceInfoData);

        // There should only be one since we're getting info for the parent device
        if (!::SetupDiEnumDeviceInfo(currDeviceInformation.get(), 0, &currDeviceInfoData)) {
            return false;
        }

        // Find out if this device is a hub device, if so, we're done
        const std::string devDescription = deviceRegistryProperty(currDeviceInformation.get(),
                                                              &currDeviceInfoData,
                                                              SPDRP_DEVICEDESC);

        if (str_contains(devDescription,"Hub")){
            hubFound = true;
        }  else {
            devInst = parentDevInst;
            devLocationStr = deviceRegistryProperty(currDeviceInformation.get(),
                                                    &currDeviceInfoData,
                                                    SPDRP_LOCATION_INFORMATION);
            count++;
        }
    }

    if (count == 3)
        // didn't find what we were looking for. Give up.
        return false;

    if (!hubFound)
        return false;

    const std::string portString("Port_#");

    if (str_contains(devLocationStr, portString))
    {
        str_remove(devLocationStr,portString);
        string devLocation_str = str_left(devLocationStr, str_indexOf(devLocationStr, "."));
        devLocation = str_toInt(devLocation_str);
    } else {
        return false;
    }

    // Build the path based on information found here:
    // https://stackoverflow.com/questions/
    // 8007468/how-do-i-obtain-usb-device-descriptor-given-a-device-path/32641140#32641140
    std::string devPath = devicePath.data();

    WCHAR guid_buf[80]{};
    StringFromGUID2(GUID_DEVINTERFACE_USB_HUB, guid_buf, 80);
    std::string hubUuidStr = "#" + wstr_to_str(guid_buf);

    str_replace(devPath, "\\", "#");
    str_prepend(devPath, "\\\\?\\");
    str_append(devPath, hubUuidStr); // GUID for hub devices
    buffer.resize(devPath.size()+1, 0);
    memcpy(buffer.data(), devPath.data(), devPath.size());

    return true;
}

struct UsbData
{
    std::string iManufacturer;
    std::string iProduct;
};

static UsbData getUSBDataFromDevice(DWORD devInst)
{
    std::vector<char> buffer;
    int devLocation;

    if (!getUSBLocationAndPath(devInst, devLocation, buffer))
        return {};

    // Open the Hub device. To get the information we want, we have to open the Hub device,
    // and pass in the port number in the argument structure.

    const HANDLE fileHandle{ ::CreateFile(buffer.data(), GENERIC_READ, FILE_SHARE_READ,
                                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                                      nullptr) };
    if (!fileHandle)
        return {};
    
    JZScopeGuard guard([fileHandle] { 
            CloseHandle(fileHandle); 
        });

    // Some of the following code was from Microsoft's sample usbview code
    constexpr ULONG size = sizeof(USB_NODE_CONNECTION_INFORMATION);
    std::array<UCHAR, size> connectionInfoData;

    // Set the port number of the device on the hub for the DeviceIoControl call.
    auto connectionInfo =
            reinterpret_cast<PUSB_NODE_CONNECTION_INFORMATION>(connectionInfoData.data());
    connectionInfo->ConnectionIndex = devLocation;

    ULONG dataSize = 0;
    if (!::DeviceIoControl(fileHandle, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION,
                          connectionInfo, size, connectionInfo, size, &dataSize, nullptr)) {
        return {};
    }

    // Get the language information, but we're only going to use the first one since we're going to
    // use this information for the manufacturer returned to Qt's caller, and there's only 1 allowed
    // there

    std::string usbStringData = getStringDescriptor(fileHandle,
                                                connectionInfo->ConnectionIndex,
                                                0, 0);

    if (usbStringData.empty())
        return {};

    USHORT languageIDs = usbStringData[0];
    UsbData usbData;

    if (connectionInfo->DeviceDescriptor.iManufacturer != 0)
        usbData.iManufacturer = getStringDescriptor(fileHandle,
                                                    connectionInfo->ConnectionIndex,
                                                    connectionInfo->DeviceDescriptor.iManufacturer,
                                                    languageIDs);

    if (connectionInfo->DeviceDescriptor.iProduct != 0)
        usbData.iProduct = getStringDescriptor(fileHandle,
                                               connectionInfo->ConnectionIndex,
                                               connectionInfo->DeviceDescriptor.iProduct,
                                               languageIDs);

    return usbData;
}

static std::string deviceInstanceIdentifier(DEVINST deviceInstanceNumber)
{
    std::vector<char> outputBuffer(MAX_DEVICE_ID_LEN + 1, 0);
    if (::CM_Get_Device_ID(
                deviceInstanceNumber,
                &outputBuffer[0],
                MAX_DEVICE_ID_LEN,
                0) != CR_SUCCESS) {
        return std::string();
    }
    return str_toUpper(&outputBuffer[0]);
}

static DEVINST parentDeviceInstanceNumber(DEVINST childDeviceInstanceNumber)
{
    ULONG nodeStatus = 0;
    ULONG problemNumber = 0;
    if (::CM_Get_DevNode_Status(&nodeStatus, &problemNumber,
                                childDeviceInstanceNumber, 0) != CR_SUCCESS) {
        return 0;
    }
    DEVINST parentInstanceNumber = 0;
    if (::CM_Get_Parent(&parentInstanceNumber, childDeviceInstanceNumber, 0) != CR_SUCCESS)
        return 0;
    return parentInstanceNumber;
}

static std::string devicePortName(HDEVINFO deviceInfoSet, PSP_DEVINFO_DATA deviceInfoData)
{
    const HKEY key = ::SetupDiOpenDevRegKey(deviceInfoSet, deviceInfoData, DICS_FLAG_GLOBAL,
                                            0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE)
        return std::string();

    static const char * const keyTokens[] = {
            "PortName\0",
            "PortNumber\0"
    };

    std::string portName;
    for (auto keyToken : keyTokens) {
        DWORD dataType = 0;
        std::vector<char> outputBuffer(MAX_PATH + 1, 0);
        DWORD bytesRequired = MAX_PATH;
        for (;;) {
            const LONG ret = ::RegQueryValueEx(key, keyToken, nullptr, &dataType,
                                               reinterpret_cast<PBYTE>(&outputBuffer[0]), &bytesRequired);
            if (ret == ERROR_MORE_DATA) {
                outputBuffer.resize(bytesRequired / sizeof(char) + 2, 0);
                continue;
            } else if (ret == ERROR_SUCCESS) {
                if (dataType == REG_SZ)
                    portName = std::string(&outputBuffer[0]);
                else if (dataType == REG_DWORD)
                    portName = str_asprintf("COM%d", *(PDWORD(&outputBuffer[0])));
            }
            break;
        }

        if (!portName.empty())
            break;
    }
    ::RegCloseKey(key);
    return portName;
}

static std::string deviceDescription(HDEVINFO deviceInfoSet,
                                 PSP_DEVINFO_DATA deviceInfoData)
{
    return deviceRegistryProperty(deviceInfoSet, deviceInfoData, SPDRP_DEVICEDESC);
}

static std::string deviceManufacturer(HDEVINFO deviceInfoSet,
                                  PSP_DEVINFO_DATA deviceInfoData)
{
    return deviceRegistryProperty(deviceInfoSet, deviceInfoData, SPDRP_MFG);
}

static uint16_t parseDeviceIdentifier(const std::string &instanceIdentifier,
                                     const std::string &identifierPrefix,
                                     int identifierSize, bool &ok)
{
    const int index = str_indexOf(instanceIdentifier, identifierPrefix);
    if (index == -1)
        return uint16_t(0);
    string ident_str = str_mid(instanceIdentifier, index + identifierPrefix.size(), identifierSize);
    return str_toInt(ident_str , & ok, 16);
}

static uint16_t deviceVendorIdentifier(const std::string &instanceIdentifier, bool &ok)
{
    static constexpr int vendorIdentifierSize = 4;
    uint16_t result = parseDeviceIdentifier(
                instanceIdentifier, ("VID_"), vendorIdentifierSize, ok);
    if (!ok)
        result = parseDeviceIdentifier(
                    instanceIdentifier, ("VEN_"), vendorIdentifierSize, ok);
    return result;
}

static uint16_t deviceProductIdentifier(const std::string &instanceIdentifier, bool &ok)
{
    static constexpr int productIdentifierSize = 4;
    uint16_t result = parseDeviceIdentifier(
                instanceIdentifier, ("PID_"), productIdentifierSize, ok);
    if (!ok)
        result = parseDeviceIdentifier(
                    instanceIdentifier, ("DEV_"), productIdentifierSize, ok);
    return result;
}

static std::string parseDeviceSerialNumber(const std::string &instanceIdentifier)
{
    int firstbound = str_lastIndexOf(instanceIdentifier, "\\");
    int lastbound = str_indexOf(instanceIdentifier, "_", firstbound);
    if (str_startsWith(instanceIdentifier ,"USB\\"))
    {
        if (lastbound != instanceIdentifier.size() - 3)
            lastbound = (int)instanceIdentifier.size();
        int ampersand = str_indexOf(instanceIdentifier, "&", firstbound);
        if (ampersand != -1 && ampersand < lastbound)
            return std::string();
    }
    else if (str_startsWith(instanceIdentifier,"FTDIBUS\\"))
    {
        firstbound = str_lastIndexOf(instanceIdentifier, "+");
        lastbound = str_indexOf(instanceIdentifier, "\\", firstbound);
        if (lastbound == -1)
            return std::string();
    } else {
        return std::string();
    }

    return str_mid(instanceIdentifier, firstbound + 1, lastbound - firstbound - 1);
}

static std::string deviceSerialNumber(std::string instanceIdentifier,
                                  DEVINST deviceInstanceNumber)
{
    for (;;) {
        const std::string result = parseDeviceSerialNumber(instanceIdentifier);
        if (!result.empty())
            return result;
        deviceInstanceNumber = parentDeviceInstanceNumber(deviceInstanceNumber);
        if (deviceInstanceNumber == 0)
            break;
        instanceIdentifier = deviceInstanceIdentifier(deviceInstanceNumber);
        if (instanceIdentifier.empty())
            break;
    }

    return std::string();
}

static bool anyOfPorts(const std::vector<JZSerialPortInfo> &ports, const std::string &portName)
{
    const auto end = ports.end();
    auto isPortNamesEquals = [&portName](const JZSerialPortInfo &portInfo) {
        return portInfo.portName() == portName;
    };
    return std::find_if(ports.begin(), end, isPortNamesEquals) != end;
}

std::vector<JZSerialPortInfo> JZSerialPortInfo::availablePorts()
{
    static const struct {
        GUID guid; DWORD flags;
    } setupTokens[] =  {
        { GUID_DEVCLASS_PORTS, DIGCF_PRESENT },
        { GUID_DEVCLASS_MODEM, DIGCF_PRESENT },
        { GUID_DEVINTERFACE_COMPORT, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE },
        { GUID_DEVINTERFACE_MODEM, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE }
    };

    std::vector<JZSerialPortInfo> serialPortInfoList;

    for (const auto& setupToken : setupTokens) {
        const DevInfoHandle deviceInfoSet{ ::SetupDiGetClassDevs(&setupToken.guid, nullptr,
                                                                 nullptr, setupToken.flags) };
        if (!deviceInfoSet)
            return serialPortInfoList;

        SP_DEVINFO_DATA deviceInfoData{};
        deviceInfoData.cbSize = sizeof(deviceInfoData);

        DWORD index = 0;
        while (::SetupDiEnumDeviceInfo(deviceInfoSet.get(), index++, &deviceInfoData)) {
            const std::string portName = devicePortName(deviceInfoSet.get(), &deviceInfoData);
            if (portName.empty() || str_contains(portName ,"LPT"))
                continue;

            if (anyOfPorts(serialPortInfoList, portName))
                continue;

            JZSerialPortInfoPrivate priv;

            priv.portName = portName;
            priv.device = JZSerialPortInfoPrivate::portNameToSystemLocation(portName);
            priv.description = deviceDescription(deviceInfoSet.get(), &deviceInfoData);
            priv.manufacturer = deviceManufacturer(deviceInfoSet.get(), &deviceInfoData);

            const std::string instanceIdentifier = deviceInstanceIdentifier(deviceInfoData.DevInst);

            priv.serialNumber =
                    deviceSerialNumber(instanceIdentifier, deviceInfoData.DevInst);
            priv.vendorIdentifier =
                    deviceVendorIdentifier(instanceIdentifier, priv.hasVendorIdentifier);
            priv.productIdentifier =
                    deviceProductIdentifier(instanceIdentifier, priv.hasProductIdentifier);

            // This makes Windows return the same data that linux does for Manufacturer and Product
            if ((str_startsWith(instanceIdentifier, "USB\\") ||
                 str_startsWith(instanceIdentifier, "FTDIBUS\\"))
                && priv.hasVendorIdentifier && priv.hasProductIdentifier) {
                UsbData usbStrings = getUSBDataFromDevice(deviceInfoData.DevInst);

                if (!usbStrings.iManufacturer.empty())
                    priv.manufacturer = usbStrings.iManufacturer;

                if (!usbStrings.iProduct.empty())
                    priv.description = usbStrings.iProduct;
                // End of making Windows return the same data as Linux
            }

            serialPortInfoList.push_back(priv);
        }
    }

    const auto portNames = portNamesFromHardwareDeviceMap();
    for (const std::string &portName : portNames) {
        if (!anyOfPorts(serialPortInfoList, portName)) {
            JZSerialPortInfoPrivate priv;
            priv.portName = portName;
            priv.device =  JZSerialPortInfoPrivate::portNameToSystemLocation(portName);
            serialPortInfoList.push_back(priv);
        }
    }

    return serialPortInfoList;
}

std::string JZSerialPortInfoPrivate::portNameToSystemLocation(const std::string &source)
{
    return str_startsWith(source, "COM")
            ? ("\\\\.\\" + source) : source;
}

std::string JZSerialPortInfoPrivate::portNameFromSystemLocation(const std::string &source)
{
    return (str_startsWith(source, "\\\\.\\")
            || str_startsWith(source,"//./"))
            ? str_mid(source,4) : source;
}