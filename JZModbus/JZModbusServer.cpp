#include "JZApplication.h"
#include "JZModbusServer.h"
#include "JZModbusRtu.h"
#include "JZModbusTcp.h"

JZModbusServer::JZModbusServer(JZObject *parent)
    :JZObject(parent), sigMappingRecved(this)
{
    m_com = nullptr;
    m_tcpServer = nullptr;
    m_ctx = new JZModbusContext();    
    m_mapping = new JZModbusMapping();

    m_baud = 9600;
    m_dataBit = JZSerialPort::Data8;
    m_stopBit = JZSerialPort::OneStop;
    m_parityBit = JZSerialPort::NoParity;
    m_port = 0;
}

JZModbusServer::~JZModbusServer()
{
    clear();
    delete m_ctx;
    delete m_mapping;
}

void JZModbusServer::initMapping(int nb_bits, int nb_input_bits,
    int nb_registers, int nb_input_registers)
{
    m_mapping->init(nb_bits, nb_input_bits, nb_registers, nb_input_registers);
}

JZModbusMapping *JZModbusServer::mapping()
{    
    return m_mapping;
}

void JZModbusServer::setSlave(int slave)
{
    m_ctx->setSlave(slave);
}

void JZModbusServer::setPlcMode(bool slave)
{
    return m_ctx->setPlcMode(slave);
}

void JZModbusServer::clear()
{
    stop();

    if (m_com)
    {
        delete m_com;
        m_com = nullptr;
    }

    if (m_tcpServer)
    {
        delete m_tcpServer;
        m_tcpServer = nullptr;
    }
}

void JZModbusServer::initConn(JZModbusConnetInfo conn)
{
    if (conn.isRtu())
        initRtu(conn.portName, conn.baud, (JZSerialPort::DataBits)conn.dataBit, (JZSerialPort::StopBits)conn.stopBit, (JZSerialPort::Parity)conn.parityBit);
    else
        initTcp(conn.port);

    setSlave(conn.slave);
    setPlcMode(conn.plcMode);
}

void JZModbusServer::initRtu(std::string com, int baud, JZSerialPort::DataBits data_bit, JZSerialPort::StopBits stop_bit, JZSerialPort::Parity parity_bit)
{   
    clear();

    m_com = new JZSerialPort(this);
    connect(&m_com->sigReadyRead, this, &JZModbusServer::onComRead);
    initRtuContext(m_ctx);

    m_comName = com;
    m_baud = baud;
    m_dataBit = data_bit;
    m_stopBit = stop_bit;
    m_parityBit = parity_bit;
}

void JZModbusServer::initTcp(int port)
{
    clear();
        
    initTcpContext(m_ctx);
    m_tcpServer = new JZTcpServer(this);
    connect(&m_tcpServer->sigAccept, this, &JZModbusServer::onNewConnect);

    m_port = port;
}

bool JZModbusServer::isStart() const
{
    if (m_com && m_com->isOpen())
        return true;
    if (m_tcpServer && m_tcpServer->isListening())
        return true;
    
    return false;
}

bool JZModbusServer::start()
{
    if (m_com)
    {
        m_com->setPortName(m_comName);
        if (!m_com->open())        
            return false;        

        m_com->setBaudRate(m_baud);
        m_com->setDataBits(m_dataBit);
        m_com->setStopBits(m_stopBit);
        m_com->setParity(m_parityBit);
        return true;
    }
    else if(m_tcpServer)
    {
        if (!m_tcpServer->listen(m_port))
            return false;

        return true;
    }

    return false;
}

void JZModbusServer::stop()
{
    if (m_com)
    {
        m_com->close();        
        m_comBuffer.clear();
    }

    if (m_tcpServer)
    {
        m_tcpServer->close();
        
        //断开所有连接
        auto socket_list = map_keys(m_tcpClients);
        for (int i = 0; i < socket_list.size(); i++)
        {
            socket_list[i]->close();
        }
        jzApp->processEvents();

        JZ_ASSERT(m_tcpClients.size() == 0);
    }
}

void JZModbusServer::onNewConnect(JZTcpSocket* socket)
{
    connect(&socket->sigReadyRead, this, &JZModbusServer::onTcpRead);
    connect(&socket->sigDisconnect, this, &JZModbusServer::onTcpDisconnected);

    m_tcpClients[socket] = JZByteArray();
}

void JZModbusServer::onTcpRead()
{
    JZTcpSocket *socket = jzobject_cast<JZTcpSocket*>(sender());
    m_tcpClients[socket].append(socket->readAll());

    auto &buffer = m_tcpClients[socket];

    uint8_t rsp[MAX_MESSAGE_LENGTH];
    int rc = dealBuffer(rsp, buffer);
    if (rc == MODBUS_WAIT_RECV)
        return;

    if (rc > 0)
    {
        rc = m_ctx->sendMsgPre(rsp, rc);
        socket->write((char*)rsp, rc);
    }
}

void JZModbusServer::onTcpDisconnected()
{
    JZTcpSocket *socket = jzobject_cast<JZTcpSocket*>(sender());
    map_remove(m_tcpClients, socket);
    socket->deleteLater();
}

void JZModbusServer::onComRead()
{
    m_comBuffer.append(m_com->readAll());

    JZByteArray &buffer = m_comBuffer;

    uint8_t rsp[MAX_MESSAGE_LENGTH];
    int rc = dealBuffer(rsp, buffer);
    if (rc == MODBUS_WAIT_RECV)
        return;
    
    if (rc > 0)
    {        
        rc = m_ctx->sendMsgPre(rsp, rc);        
        m_com->write((char*)rsp, rc);
    }
    else
    {

    }
}

int JZModbusServer::dealBuffer(uint8_t *rsp, JZByteArray &buffer)
{
    int rc = m_ctx->receiveMessage(MSG_INDICATION, buffer);
    if (rc == MODBUS_WAIT_RECV)
        return rc;
    else if (rc > 0)
    {
        JZByteArray res = buffer.left(rc);
        buffer = buffer.mid(rc);

        const uint8_t *req = (const uint8_t *)res.constData();
        int slave = req[m_ctx->backend->header_length - 1];
        
        MappingChanged info;
        int rsp_length = m_ctx->reply(rsp, req, res.size(), m_mapping, info);
        if (info.addr != -1)
            sigMappingRecved.emit(info.addr, info.nb);

        return rsp_length;
    }
    else
    {
        buffer.clear();
        return 0;
    }
}