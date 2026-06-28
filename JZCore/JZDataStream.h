#ifndef JZ_DATA_STREAM_H_
#define JZ_DATA_STREAM_H_

#include <string>
#include <stdint.h>
#include <type_traits>
#include <map>
#include "JZByteArray.h"
#include "JZFile.h"

class JZDataStreamImpl;
class JZDataStream
{
public:
    JZDataStream(JZFile *file);
    JZDataStream(JZByteArray *buffer);
    JZDataStream(const JZByteArray &buffer);    
    ~JZDataStream();

    int writeRawData(const char *s, int len);
    int readRawData(char *s, int len);
    void skipRawData(int offset);

    JZDataStream &operator<<(bool i);
    JZDataStream &operator>>(bool &i);
    JZDataStream &operator<<(int8_t i);
    JZDataStream &operator>>(int8_t &i);
    JZDataStream &operator<<(uint8_t i);
    JZDataStream &operator>>(uint8_t &i);
    JZDataStream &operator<<(int16_t i);
    JZDataStream &operator>>(int16_t &i);
    JZDataStream &operator<<(uint16_t i);
    JZDataStream &operator>>(uint16_t &i);
    JZDataStream &operator<<(int32_t i);
    JZDataStream &operator>>(int32_t &i);
    JZDataStream &operator<<(uint32_t i);
    JZDataStream &operator>>(uint32_t &i);
    JZDataStream &operator<<(int64_t i);
    JZDataStream &operator>>(int64_t &i);
    JZDataStream &operator<<(uint64_t i);
    JZDataStream &operator>>(uint64_t &i);
    JZDataStream &operator<<(float i);
    JZDataStream &operator>>(float &i);
    JZDataStream &operator<<(double i);
    JZDataStream &operator>>(double &i);

protected:
    JZ_DISABLE_COPY(JZDataStream);
    
    template<class T>
    void writePod(T i)
    {
        if (m_file)
        {
            m_file->write((const char*)&i, sizeof(i));
        }
        else
        {
            if (m_offset + (int)sizeof(i) > m_buffer->size())
                m_buffer->resize(m_offset + sizeof(i));

            T *ptr = (T*)(m_buffer->data() + m_offset);
            *ptr = i;
            m_offset += sizeof(i);
        }
    }

    template<class T>
    void readPod(T &i)
    {
        if (m_file)
        {
            m_file->read((char*)&i, sizeof(i));
        }
        else
        {
            if (m_offset + (int)sizeof(i) > m_buffer->size())
            {
                return;
            }

            T *ptr = (T*)(m_buffer->data() + m_offset);
            i = *ptr;
            m_offset += sizeof(i);
        }
    }

    int m_offset;
    JZByteArray *m_buffer;
    JZFile *m_file;    
};

JZDataStream &operator<<(JZDataStream &s, const std::string &str);
JZDataStream &operator>>(JZDataStream &s, std::string &str);
JZDataStream &operator<<(JZDataStream &s, const JZByteArray &buffer);
JZDataStream &operator>>(JZDataStream &s, JZByteArray &buffer);

template<class T>
JZDataStream &operator<<(JZDataStream &s, const std::vector<T> &vec)
{
    s << (int32_t)vec.size();
    if (std::is_pod<T>::value)
    {
        s.writeRawData((const char*)vec.data(),int(vec.size() * sizeof(T)));
    }
    else
    {
        for (int i = 0; i < (int)vec.size(); i++) 
            s << vec[i];
    }
    
    return s;
}

template<class T>
JZDataStream &operator>>(JZDataStream &s, std::vector<T> &vec)
{
    int32_t size = 0;
    s >> size;
    if (std::is_pod<T>::value)
    {
        vec.resize(size);
        s.readRawData((char*)vec.data(),int(vec.size() * sizeof(T)));
    }
    else
    {
        for (int i = 0; i < size; i++)
        {
            T t;
            s >> t;
            vec.push_back(t);            
        }
    }

    return s;
}


template<class K, class V>
JZDataStream &operator<<(JZDataStream &s, const std::map<K, V> &m)
{
    s << (int32_t)m.size();
    auto it = m.begin();
    while (it != m.end())
    {
        s << it->first;
        s << it->second;
        it++;
    }

    return s;
}

template<class K, class V>
JZDataStream &operator >> (JZDataStream &s, std::map<K,V> &vec)
{
    int32_t size = 0;
    s >> size;

    for (int i = 0; i < size; i++)
    {
        K k;
        V v;
        s >> k;
        s >> v;
        vec[k] = v;
    }    

    return s;
}

#endif // !JZ_DATA_STREAM_H_
