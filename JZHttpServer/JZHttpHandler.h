#ifndef JZ_HTTP_HANDLER_H_
#define JZ_HTTP_HANDLER_H_

#include "JZObject.h"
#include "JZHttpRequest.h"
#include "JZHttpRespone.h"

class JZHttpServerClient;

class JZHttpHandler : public JZObject
{
public:
    virtual ~JZHttpHandler() = default;
    virtual void handle(JZHttpServerClient* client, const JZHttpRequest& req) = 0;
};

#endif // JZ_HTTP_HANDLER_H_
