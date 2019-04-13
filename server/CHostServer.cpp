/*
 * Copyright (C) 2015   Jeremy Chen jeremy_cz@yahoo.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "CHostServer.h"
#include <common_base/CFdbContext.h>
#include <common_base/CFdbMessage.h>
#include <common_base/CBaseSocketFactory.h>
#include <common_base/CFdbSession.h>
#include "CNsConfig.h"
#include <utils/Log.h>

CHostServer::CHostServer()
    : CBaseServer(CNsConfig::getHostServerName())
    , mHeartBeatTimer(this)
{
    mMsgHdl.registerCallback(NFdbBase::REQ_REGISTER_HOST, &CHostServer::onRegisterHostReq);
    mMsgHdl.registerCallback(NFdbBase::REQ_UNREGISTER_HOST, &CHostServer::onUnregisterHostReq);
    mMsgHdl.registerCallback(NFdbBase::REQ_QUERY_HOST, &CHostServer::onQueryHostReq);
    mMsgHdl.registerCallback(NFdbBase::REQ_HEARTBEAT_OK, &CHostServer::onHeartbeatOk);

    mSubscribeHdl.registerCallback(NFdbBase::NTF_HOST_ONLINE, &CHostServer::onHostOnlineReg);
    mHeartBeatTimer.attach(FDB_CONTEXT, false);
}

CHostServer::~CHostServer()
{
}

void CHostServer::onSubscribe(CBaseJob::Ptr &msg_ref)
{
    CFdbMessage *msg = castToMessage<CFdbMessage *>(msg_ref);
    const ::NFdbBase::FdbMsgSubscribeItem *sub_item;
    FDB_BEGIN_FOREACH_SIGNAL(msg, sub_item)
    {
        mSubscribeHdl.processMessage(this, msg, sub_item, sub_item->msg_code());
    }
    FDB_END_FOREACH_SIGNAL()
}

void CHostServer::onInvoke(CBaseJob::Ptr &msg_ref)
{
    mMsgHdl.processMessage(this, msg_ref);
}

void CHostServer::onOffline(FdbSessionId_t sid, bool is_last)
{
    tHostTbl::iterator it = mHostTbl.find(sid);
    if (it != mHostTbl.end())
    {
        CHostInfo &info = it->second;
        LOG_I("CHostServer: host is dropped: name: %s, ip: %s, ns: %s\n", info.mHostName.c_str(),
                                                                          info.mIpAddress.c_str(),
                                                                          info.mNsUrl.c_str());
        broadcastSingleHost(sid, false, info);
        mHostTbl.erase(it);
        if (mHostTbl.size() == 0)
        {
            mHeartBeatTimer.disable();
        }
    }
}

void CHostServer::onRegisterHostReq(CBaseJob::Ptr &msg_ref)
{
    CFdbMessage *msg = castToMessage<CFdbMessage *>(msg_ref);
    NFdbBase::FdbMsgHostAddress host_addr;
    if (!msg->deserialize(host_addr))
    {
        msg->status(msg_ref, NFdbBase::FDB_ST_MSG_DECODE_FAIL);
        return;
    }
    const char *ip_addr = host_addr.ip_address().c_str();
    const char *host_name = ip_addr;
    if (host_addr.has_host_name())
    {
        host_name = host_addr.host_name().c_str();
    }
    std::string str_ns_url;
    const char *ns_url;
    if (host_addr.has_ns_url())
    {
        ns_url = host_addr.ns_url().c_str();
    }
    else
    {
        CBaseSocketFactory::buildUrl(str_ns_url, FDB_SOCKET_TCP, ip_addr, CNsConfig::getNameServerTcpPort());
        ns_url = str_ns_url.c_str();
    }
    LOG_I("HostServer: host is registered: name: %s; ip: %s, ns: %s\n.", host_name, ip_addr, ns_url);
    for (tHostTbl::iterator it = mHostTbl.begin(); it != mHostTbl.end(); ++it)
    {
        if (it->second.mIpAddress == ip_addr)
        {
            msg->status(msg_ref, NFdbBase::FDB_ST_ALREADY_EXIST);
            return;
        }
    }

    CHostInfo &info = mHostTbl[msg->session()];
    info.mHostName = host_name;
    info.mIpAddress = ip_addr;
    info.mNsUrl = ns_url;
    info.mHbCount = 0;

    broadcastSingleHost(msg->session(), true, info);
    if (mHostTbl.size() == 1)
    {
        mHeartBeatTimer.enable();
    }
}

void CHostServer::broadcastSingleHost(FdbSessionId_t sid, bool online, CHostInfo &info)
{
    ::NFdbBase::FdbMsgHostAddressList addr_list;
    ::NFdbBase::FdbMsgHostAddress *addr = addr_list.add_address_list();
    addr->set_host_name(info.mHostName);
    addr->set_ip_address(info.mIpAddress);
    addr->set_ns_url(online ? info.mNsUrl : ""); // ns_url being empty means offline

    broadcast(NFdbBase::NTF_HOST_ONLINE, addr_list);
}

void CHostServer::onUnregisterHostReq(CBaseJob::Ptr &msg_ref)
{
    CFdbMessage *msg = castToMessage<CFdbMessage *>(msg_ref);
    NFdbBase::FdbMsgHostAddress host_addr;
    if (!msg->deserialize(host_addr))
    {
        msg->status(msg_ref, NFdbBase::FDB_ST_MSG_DECODE_FAIL);
        return;
    }
    const char *ip_addr = host_addr.ip_address().c_str();

    for (tHostTbl::iterator it = mHostTbl.begin(); it != mHostTbl.end(); ++it)
    {
        if (it->second.mIpAddress == ip_addr)
        {
            CHostInfo &info = it->second;
            LOG_I("CHostServer: host is unregistered: name: %s, ip: %s, ns: %s\n", info.mHostName.c_str(),
                                                                                   info.mIpAddress.c_str(),
                                                                                   info.mNsUrl.c_str());
            broadcastSingleHost(msg->session(), false, info);
            mHostTbl.erase(it);
            return;
        }
    }
    msg->status(msg_ref, NFdbBase::FDB_ST_NON_EXIST);
}

void CHostServer::onQueryHostReq(CBaseJob::Ptr &msg_ref)
{
    CFdbMessage *msg = castToMessage<CFdbMessage *>(msg_ref);
    msg->status(msg_ref, NFdbBase::FDB_ST_NOT_IMPLEMENTED, "onQueryHostReq() is not implemented!");
}

void CHostServer::onHeartbeatOk(CBaseJob::Ptr &msg_ref)
{
    CFdbMessage *msg = castToMessage<CFdbMessage *>(msg_ref);
    tHostTbl::iterator it = mHostTbl.find(msg->session());

    if (it != mHostTbl.end())
    {
        CHostInfo &info = it->second;
        info.mHbCount = 0;
    }
}

void CHostServer::onHostOnlineReg(CFdbMessage *msg, const ::NFdbBase::FdbMsgSubscribeItem *sub_item)
{
    ::NFdbBase::FdbMsgHostAddressList addr_list;
    for (tHostTbl::iterator it = mHostTbl.begin(); it != mHostTbl.end(); ++it)
    {
        CHostInfo &info = it->second;

        ::NFdbBase::FdbMsgHostAddress *addr = addr_list.add_address_list();
        addr->set_ip_address(info.mIpAddress);
        addr->set_ns_url(info.mNsUrl); // ns_url being empty means offline
        addr->set_host_name(info.mHostName);
    }
    if (!addr_list.address_list().empty())
    {
        msg->broadcast(NFdbBase::NTF_HOST_ONLINE, addr_list);
    }
}

void CHostServer::broadcastHeartBeat(CMethodLoopTimer<CHostServer> *timer)
{
    for (tHostTbl::iterator it = mHostTbl.begin(); it != mHostTbl.end();)
    {
        tHostTbl::iterator the_it = it++;
        CHostInfo &info = the_it->second;
        if (++info.mHbCount >= CNsConfig::getHeartBeatRetryNr())
        {
            // will trigger offline callback which do everything for me.
            LOG_E("Host %s is kicked out due to HB!\n", info.mHostName.c_str());
            kickOut(the_it->first);
        }
    }
    broadcast(NFdbBase::NTF_HEART_BEAT);
}

