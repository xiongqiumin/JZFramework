#ifndef JZ_NET_DEFINE_H_
#define JZ_NET_DEFINE_H_

#include "JZLog.h"

#if 0
#define LOG_NET_D(...)  LOG_FMT_D(__VA_ARGS__)
#define LOG_NET_W(...)  LOG_FMT_W(__VA_ARGS__)
#define LOG_NET_E(...)  LOG_FMT_E(__VA_ARGS__)
#else
#define LOG_NET_D(...)
#define LOG_NET_W(...)
#define LOG_NET_E(...)
#endif

struct mg_mgr;
struct mg_connection;
struct mg_tls_opts;

#endif // !JZ_NET_DEFINE_H_