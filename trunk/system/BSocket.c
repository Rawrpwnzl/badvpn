/**
 * @file BSocket.c
 * @author Ambroz Bizjak <ambrop7@gmail.com>
 * 
 * @section LICENSE
 * 
 * This file is part of BadVPN.
 * 
 * BadVPN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 * 
 * BadVPN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef BADVPN_USE_WINAPI
#include <winsock2.h>
#include <misc/mswsock.h>
#else
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <misc/debug.h>

#include <system/BSocket.h>

#define HANDLER_READ 0
#define HANDLER_WRITE 1
#define HANDLER_ACCEPT 2
#define HANDLER_CONNECT 3

static void init_handlers (BSocket *bs)
{
    bs->global_handler = NULL;
    
    int i;
    for (i = 0; i < 4; i++) {
        bs->handlers[i] = NULL;
    }
}

static int set_nonblocking (int s)
{
    #ifdef BADVPN_USE_WINAPI
    unsigned long bl = 1;
    int res = ioctlsocket(s, FIONBIO, &bl);
    #else
    int res = fcntl(s, F_SETFL, O_NONBLOCK);
    #endif
    return res;
}

static int set_pktinfo (int s)
{
    #ifdef BADVPN_USE_WINAPI
    DWORD opt = 1;
    int res = setsockopt(s, IPPROTO_IP, IP_PKTINFO, (char *)&opt, sizeof(opt));
    #else
    int opt = 1;
    int res = setsockopt(s, IPPROTO_IP, IP_PKTINFO, &opt, sizeof(opt));
    #endif
    return res;
}

static int set_pktinfo6 (int s)
{
    #ifdef BADVPN_USE_WINAPI
    DWORD opt = 1;
    int res = setsockopt(s, IPPROTO_IPV6, IPV6_PKTINFO, (char *)&opt, sizeof(opt));
    #else
    int opt = 1;
    int res = setsockopt(s, IPPROTO_IPV6, IPV6_RECVPKTINFO, &opt, sizeof(opt));
    #endif
    return res;
}

static void close_socket (int fd)
{
    #ifdef BADVPN_USE_WINAPI
    int res = closesocket(fd);
    #else
    int res = close(fd);
    #endif
    ASSERT_FORCE(res == 0)
}

struct sys_addr {
    #ifdef BADVPN_USE_WINAPI
    int len;
    #else
    socklen_t len;
    #endif
    union {
        struct sockaddr generic;
        struct sockaddr_in ipv4;
        struct sockaddr_in6 ipv6;
    } addr;
};

static void addr_socket_to_sys (struct sys_addr *out, BAddr *addr)
{
    switch (addr->type) {
        case BADDR_TYPE_IPV4:
            out->len = sizeof(out->addr.ipv4);
            memset(&out->addr.ipv4, 0, sizeof(out->addr.ipv4));
            out->addr.ipv4.sin_family = AF_INET;
            out->addr.ipv4.sin_port = addr->ipv4.port;
            out->addr.ipv4.sin_addr.s_addr = addr->ipv4.ip;
            break;
        case BADDR_TYPE_IPV6:
            out->len = sizeof(out->addr.ipv6);
            memset(&out->addr.ipv6, 0, sizeof(out->addr.ipv6));
            out->addr.ipv6.sin6_family = AF_INET6;
            out->addr.ipv6.sin6_port = addr->ipv6.port;
            out->addr.ipv6.sin6_flowinfo = 0;
            memcpy(out->addr.ipv6.sin6_addr.s6_addr, addr->ipv6.ip, 16);
            out->addr.ipv6.sin6_scope_id = 0;
            break;
        default:
            ASSERT(0)
            break;
    }
}

static void addr_sys_to_socket (BAddr *out, struct sys_addr *addr)
{
    switch (addr->addr.generic.sa_family) {
        case AF_INET:
            ASSERT(addr->len == sizeof(struct sockaddr_in))
            out->type = BADDR_TYPE_IPV4;
            out->ipv4.ip = addr->addr.ipv4.sin_addr.s_addr;
            out->ipv4.port = addr->addr.ipv4.sin_port;
            break;
        case AF_INET6:
            ASSERT(addr->len == sizeof(struct sockaddr_in6))
            out->type = BADDR_TYPE_IPV6;
            memcpy(out->ipv6.ip, addr->addr.ipv6.sin6_addr.s6_addr, 16);
            out->ipv6.port = addr->addr.ipv6.sin6_port;
            break;
        default:
            ASSERT(0)
            break;
    }
}

static int get_event_index (int event)
{
    switch (event) {
        case BSOCKET_READ:
            return HANDLER_READ;
        case BSOCKET_WRITE:
            return HANDLER_WRITE;
        case BSOCKET_ACCEPT:
            return HANDLER_ACCEPT;
        case BSOCKET_CONNECT:
            return HANDLER_CONNECT;
        default:
            ASSERT(0)
            return 42;
    }
}

static void call_handlers (BSocket *bs, int returned_events)
{
    // reset recv number
    bs->recv_num = 0;
    
    if (bs->global_handler) {
        bs->global_handler(bs->global_handler_user, returned_events);
        return;
    }
    
    if (returned_events&BSOCKET_READ) {
        ASSERT(bs->handlers[HANDLER_READ])
        DEAD_ENTER(bs->dead)
        bs->handlers[HANDLER_READ](bs->handlers_user[HANDLER_READ], BSOCKET_READ);
        if (DEAD_LEAVE(bs->dead)) {
            return;
        }
    }
    if (returned_events&BSOCKET_WRITE) {
        ASSERT(bs->handlers[HANDLER_WRITE])
        DEAD_ENTER(bs->dead)
        bs->handlers[HANDLER_WRITE](bs->handlers_user[HANDLER_WRITE], BSOCKET_WRITE);
        if (DEAD_LEAVE(bs->dead)) {
            return;
        }
    }
    if (returned_events&BSOCKET_ACCEPT) {
        ASSERT(bs->handlers[HANDLER_ACCEPT])
        DEAD_ENTER(bs->dead)
        bs->handlers[HANDLER_ACCEPT](bs->handlers_user[HANDLER_ACCEPT], BSOCKET_ACCEPT);
        if (DEAD_LEAVE(bs->dead)) {
            return;
        }
    }
    if (returned_events&BSOCKET_CONNECT) {
        ASSERT(bs->handlers[HANDLER_CONNECT])
        DEAD_ENTER(bs->dead)
        bs->handlers[HANDLER_CONNECT](bs->handlers_user[HANDLER_CONNECT], BSOCKET_CONNECT);
        if (DEAD_LEAVE(bs->dead)) {
            return;
        }
    }
}

#ifdef BADVPN_USE_WINAPI

static long get_wsa_events (int sock_events)
{
    long res = 0;

    if ((sock_events&BSOCKET_READ)) {
        res |= FD_READ | FD_CLOSE;
    }
    if ((sock_events&BSOCKET_WRITE)) {
        res |= FD_WRITE | FD_CLOSE;
    }
    if ((sock_events&BSOCKET_ACCEPT)) {
        res |= FD_ACCEPT;
    }
    if ((sock_events&BSOCKET_CONNECT)) {
        res |= FD_CONNECT;
    }

    return res;
}

static void handle_handler (BSocket *bs)
{
    // enumerate network events and reset event
    WSANETWORKEVENTS events;
    int res = WSAEnumNetworkEvents(bs->socket, bs->event, &events);
    ASSERT_FORCE(res == 0)
    
    int returned_events = 0;
    
    if (bs->waitEvents&BSOCKET_READ) {
        if ((events.lNetworkEvents&FD_READ) || (events.lNetworkEvents&FD_CLOSE)) {
            returned_events |= BSOCKET_READ;
        }
    }
    
    if (bs->waitEvents&BSOCKET_WRITE) {
        if ((events.lNetworkEvents&FD_WRITE) || (events.lNetworkEvents&FD_CLOSE)) {
            returned_events |= BSOCKET_WRITE;
        }
    }
    
    if (bs->waitEvents&BSOCKET_ACCEPT) {
        if (events.lNetworkEvents&FD_ACCEPT) {
            returned_events |= BSOCKET_ACCEPT;
        }
    }
    
    if (bs->waitEvents&BSOCKET_CONNECT) {
        if (events.lNetworkEvents&FD_CONNECT) {
            // read connection attempt result
            ASSERT(bs->connecting_status == 1)
            bs->connecting_status = 2;
            switch (events.iErrorCode[FD_CONNECT_BIT]) {
                case 0:
                    bs->connecting_result = BSOCKET_ERROR_NONE;
                    break;
                case WSAETIMEDOUT:
                    bs->connecting_result = BSOCKET_ERROR_CONNECTION_TIMED_OUT;
                    break;
                case WSAECONNREFUSED:
                    bs->connecting_result = BSOCKET_ERROR_CONNECTION_REFUSED;
                    break;
                default:
                    bs->connecting_result = BSOCKET_ERROR_UNKNOWN;
            }
            
            returned_events |= BSOCKET_CONNECT;
        }
    }
    
    call_handlers(bs, returned_events);
}

#else

static int get_reactor_fd_events (int sock_events)
{
    int res = 0;

    if ((sock_events&BSOCKET_READ) || (sock_events&BSOCKET_ACCEPT)) {
        res |= BREACTOR_READ;
    }
    if ((sock_events&BSOCKET_WRITE) || (sock_events&BSOCKET_CONNECT)) {
        res |= BREACTOR_WRITE;
    }

    return res;
}

static void file_descriptor_handler (BSocket *bs, int events)
{
    int returned_events = 0;
    
    if ((bs->waitEvents&BSOCKET_READ) && (events&BREACTOR_READ)) {
        returned_events |= BSOCKET_READ;
    }
    
    if ((bs->waitEvents&BSOCKET_WRITE) && (events&BREACTOR_WRITE)) {
        returned_events |= BSOCKET_WRITE;
    }
    
    if ((bs->waitEvents&BSOCKET_ACCEPT) && (events&BREACTOR_READ)) {
        returned_events |= BSOCKET_ACCEPT;
    }
    
    if ((bs->waitEvents&BSOCKET_CONNECT) && (events&BREACTOR_WRITE)) {
        returned_events |= BSOCKET_CONNECT;
        
        // read connection attempt result
        ASSERT(bs->connecting_status == 1)
        bs->connecting_status = 2;
        int result;
        socklen_t result_len = sizeof(result);
        int res = getsockopt(bs->socket, SOL_SOCKET, SO_ERROR, &result, &result_len);
        ASSERT_FORCE(res == 0)
        switch (result) {
            case 0:
                bs->connecting_result = BSOCKET_ERROR_NONE;
                break;
            case ETIMEDOUT:
                bs->connecting_result = BSOCKET_ERROR_CONNECTION_TIMED_OUT;
                break;
            case ECONNREFUSED:
                bs->connecting_result = BSOCKET_ERROR_CONNECTION_REFUSED;
                break;
            default:
                bs->connecting_result = BSOCKET_ERROR_UNKNOWN;
        }
    }
    
    call_handlers(bs, returned_events);
}

#endif

static int init_event_backend (BSocket *bs)
{
    #ifdef BADVPN_USE_WINAPI
    if ((bs->event = WSACreateEvent()) == WSA_INVALID_EVENT) {
        return 0;
    }
    BHandle_Init(&bs->bhandle, bs->event, (BHandle_handler)handle_handler, bs);
    if (!BReactor_AddHandle(bs->bsys, &bs->bhandle)) {
        ASSERT_FORCE(WSACloseEvent(bs->event))
        return 0;
    }
    BReactor_EnableHandle(bs->bsys, &bs->bhandle);
    #else
    BFileDescriptor_Init(&bs->fd, bs->socket, (BFileDescriptor_handler)file_descriptor_handler, bs);
    if (!BReactor_AddFileDescriptor(bs->bsys, &bs->fd)) {
        return 0;
    }
    #endif
    
    return 1;
}

static void free_event_backend (BSocket *bs)
{
    #ifdef BADVPN_USE_WINAPI
    BReactor_RemoveHandle(bs->bsys, &bs->bhandle);
    ASSERT_FORCE(WSACloseEvent(bs->event))
    #else
    BReactor_RemoveFileDescriptor(bs->bsys, &bs->fd);
    #endif
}

static void update_event_backend (BSocket *bs)
{
    #ifdef BADVPN_USE_WINAPI
    ASSERT_FORCE(WSAEventSelect(bs->socket, bs->event, get_wsa_events(bs->waitEvents)) == 0)
    #else
    BReactor_SetFileDescriptorEvents(bs->bsys, &bs->fd, get_reactor_fd_events(bs->waitEvents));
    #endif
}

static int limit_recv (BSocket *bs)
{
    if (bs->recv_max > 0) {
        if (bs->recv_num >= bs->recv_max) {
            return 1;
        }
        bs->recv_num++;
    }
    
    return 0;
}

int BSocket_GlobalInit (void)
{
    #ifdef BADVPN_USE_WINAPI
    
    WORD requested = MAKEWORD(2, 2);
    WSADATA wsadata;
    if (WSAStartup(requested, &wsadata) != 0) {
        goto fail0;
    }
    if (wsadata.wVersion != requested) {
        goto fail1;
    }
    
    return 0;
    
fail1:
    WSACleanup();
fail0:
    return -1;
    
    #else
    
    return 0;
    
    #endif
}

int BSocket_Init (BSocket *bs, BReactor *bsys, int domain, int type)
{
    // translate domain
    int sys_domain;
    switch (domain) {
        case BADDR_TYPE_IPV4:
            sys_domain = AF_INET;
            break;
        case BADDR_TYPE_IPV6:
            sys_domain = AF_INET6;
            break;
        default:
            ASSERT(0)
            return -1;
    }

    // translate type
    int sys_type;
    switch (type) {
        case BSOCKET_TYPE_STREAM:
            sys_type = SOCK_STREAM;
            break;
        case BSOCKET_TYPE_DGRAM:
            sys_type = SOCK_DGRAM;
            break;
        default:
            ASSERT(0)
            return -1;
    }

    // create socket
    int fd = socket(sys_domain, sys_type, 0);
    if (fd < 0) {
        DEBUG("socket() failed");
        goto fail0;
    }

    // set socket nonblocking
    if (set_nonblocking(fd) != 0) {
        DEBUG("set_nonblocking failed");
        goto fail1;
    }
    
    // set pktinfo option
    int have_pktinfo = 0;
    if (type == BSOCKET_TYPE_DGRAM) {
        switch (domain) {
            case BADDR_TYPE_IPV4:
                have_pktinfo = (set_pktinfo(fd) == 0);
                break;
            case BADDR_TYPE_IPV6:
                have_pktinfo = (set_pktinfo6(fd) == 0);
                break;
        }
        if (!have_pktinfo) {
            DEBUG("WARNING: no pktinfo");
        }
    }
    
    // initialize variables
    DEAD_INIT(bs->dead);
    bs->bsys = bsys;
    bs->type = type;
    bs->socket = fd;
    bs->have_pktinfo = have_pktinfo;
    bs->error = BSOCKET_ERROR_NONE;
    init_handlers(bs);
    bs->waitEvents = 0;
    bs->connecting_status = 0;
    bs->recv_max = BSOCKET_DEFAULT_RECV_MAX;
    bs->recv_num = 0;
    
    // initialize event backend
    if (!init_event_backend(bs)) {
        DEBUG("WARNING: init_event_backend failed");
        goto fail1;
    }
    
    // init debug object
    DebugObject_Init(&bs->d_obj);
    
    return 0;
    
fail1:
    close_socket(fd);
fail0:
    return -1;
}

void BSocket_Free (BSocket *bs)
{
    // free debug object
    DebugObject_Free(&bs->d_obj);
    
    // free event backend
    free_event_backend(bs);
    
    // close socket
    close_socket(bs->socket);
    
    // if we're being called indirectly from a socket event handler,
    // allow the function invoking the handler to know that the socket was freed
    DEAD_KILL(bs->dead);
}

void BSocket_SetRecvMax (BSocket *bs, int max)
{
    ASSERT(max > 0 || max == -1)
    
    bs->recv_max = max;
    bs->recv_num = 0;
}

int BSocket_GetError (BSocket *bs)
{
    return bs->error;
}

void BSocket_AddGlobalEventHandler (BSocket *bs, BSocket_handler handler, void *user)
{
    ASSERT(handler)
    ASSERT(!bs->global_handler)
    ASSERT(!bs->handlers[0])
    ASSERT(!bs->handlers[1])
    ASSERT(!bs->handlers[2])
    ASSERT(!bs->handlers[3])
    
    bs->global_handler = handler;
    bs->global_handler_user = user;
}

void BSocket_RemoveGlobalEventHandler (BSocket *bs)
{
    ASSERT(bs->global_handler)
    
    bs->global_handler = NULL;
    bs->waitEvents = 0;
}

void BSocket_SetGlobalEvents (BSocket *bs, int events)
{
    ASSERT(bs->global_handler)
    
    // update events
    bs->waitEvents = events;
    
    // give new events to event backend
    update_event_backend(bs);
}

void BSocket_AddEventHandler (BSocket *bs, uint8_t event, BSocket_handler handler, void *user)
{
    ASSERT(handler)
    ASSERT(!bs->global_handler)
    
    // get index
    int i = get_event_index(event);
    
    // event must not have handler
    ASSERT(!bs->handlers[i])
    
    // change handler
    bs->handlers[i] = handler;
    bs->handlers_user[i] = user;
}

void BSocket_RemoveEventHandler (BSocket *bs, uint8_t event)
{
    // get table index
    int i = get_event_index(event);
    
    // event must have handler
    ASSERT(bs->handlers[i])
    
    // disable event if enabled
    if (bs->waitEvents&event) {
        BSocket_DisableEvent(bs, event);
    }
    
    // change handler
    bs->handlers[i] = NULL;
}

void BSocket_EnableEvent (BSocket *bs, uint8_t event)
{
    #ifndef NDEBUG
    // check event and incompatible events
    switch (event) {
        case BSOCKET_READ:
        case BSOCKET_WRITE:
            ASSERT(!(bs->waitEvents&BSOCKET_ACCEPT))
            ASSERT(!(bs->waitEvents&BSOCKET_CONNECT))
            break;
        case BSOCKET_ACCEPT:
            ASSERT(!(bs->waitEvents&BSOCKET_READ))
            ASSERT(!(bs->waitEvents&BSOCKET_WRITE))
            ASSERT(!(bs->waitEvents&BSOCKET_CONNECT))
            break;
        case BSOCKET_CONNECT:
            ASSERT(!(bs->waitEvents&BSOCKET_READ))
            ASSERT(!(bs->waitEvents&BSOCKET_WRITE))
            ASSERT(!(bs->waitEvents&BSOCKET_ACCEPT))
            break;
        default:
            ASSERT(0)
            break;
    }
    #endif
    
    // event must have handler
    ASSERT(bs->handlers[get_event_index(event)])
    
    // event must not be enabled
    ASSERT(!(bs->waitEvents&event))
    
    // update events
    bs->waitEvents |= event;
    
    // give new events to event backend
    update_event_backend(bs);
}

void BSocket_DisableEvent (BSocket *bs, uint8_t event)
{
    // check event and get index
    int index = get_event_index(event);
    
    // event must have handler
    ASSERT(bs->handlers[index])
    
    // event must be enabled
    ASSERT(bs->waitEvents&event)
    
    // update events
    bs->waitEvents &= ~event;
    
    // give new events to event backend
    update_event_backend(bs);
}

int BSocket_Connect (BSocket *bs, BAddr *addr)
{
    ASSERT(bs->connecting_status == 0)

    struct sys_addr sysaddr;
    addr_socket_to_sys(&sysaddr, addr);

    if (connect(bs->socket, &sysaddr.addr.generic, sysaddr.len) < 0) {
        #ifdef BADVPN_USE_WINAPI
        switch (WSAGetLastError()) {
            case WSAEWOULDBLOCK:
                bs->connecting_status = 1;
                bs->error = BSOCKET_ERROR_IN_PROGRESS;
                return -1;
        }
        #else
        switch (errno) {
            case EINPROGRESS:
                bs->connecting_status = 1;
                bs->error = BSOCKET_ERROR_IN_PROGRESS;
                return -1;
        }
        #endif
        
        bs->error = BSOCKET_ERROR_UNKNOWN;
        return -1;
    }

    bs->error = BSOCKET_ERROR_NONE;
    return 0;
}

int BSocket_GetConnectResult (BSocket *bs)
{
    ASSERT(bs->connecting_status == 2)

    bs->connecting_status = 0;
    
    return bs->connecting_result;
}

int BSocket_Bind (BSocket *bs, BAddr *addr)
{
    struct sys_addr sysaddr;
    addr_socket_to_sys(&sysaddr, addr);
    
    if (bs->type == BSOCKET_TYPE_STREAM) {
        #ifdef BADVPN_USE_WINAPI
        BOOL optval = TRUE;
        #else
        int optval = 1;
        #endif
        if (setsockopt(bs->socket, SOL_SOCKET, SO_REUSEADDR, (void *)&optval, sizeof(optval)) < 0) {
            DEBUG("WARNING: setsockopt failed");
        }
    }
    
    if (bind(bs->socket, &sysaddr.addr.generic, sysaddr.len) < 0) {
        #ifdef BADVPN_USE_WINAPI
        switch (WSAGetLastError()) {
            case WSAEADDRNOTAVAIL:
                bs->error = BSOCKET_ERROR_ADDRESS_NOT_AVAILABLE;
                return -1;
            case WSAEADDRINUSE:
                bs->error = BSOCKET_ERROR_ADDRESS_IN_USE;
                return -1;
        }
        #else
        switch (errno) {
            case EADDRNOTAVAIL:
                bs->error = BSOCKET_ERROR_ADDRESS_NOT_AVAILABLE;
                return -1;
            case EADDRINUSE:
                bs->error = BSOCKET_ERROR_ADDRESS_IN_USE;
                return -1;
            case EACCES:
                bs->error = BSOCKET_ERROR_ACCESS_DENIED;
                return -1;
        }
        #endif
        
        bs->error = BSOCKET_ERROR_UNKNOWN;
        return -1;
    }
    
    bs->error = BSOCKET_ERROR_NONE;
    return 0;
}

int BSocket_Listen (BSocket *bs, int backlog)
{
    if (backlog < 0) {
        backlog = BSOCKET_DEFAULT_BACKLOG;
    }
    
    if (listen(bs->socket, backlog) < 0) {
        #ifdef BADVPN_USE_WINAPI
        switch (WSAGetLastError()) {
            case WSAEADDRINUSE:
                bs->error = BSOCKET_ERROR_ADDRESS_IN_USE;
                return -1;
        }
        #else
        switch (errno) {
            case EADDRINUSE:
                bs->error = BSOCKET_ERROR_ADDRESS_IN_USE;
                return -1;
        }
        #endif
        
        bs->error = BSOCKET_ERROR_UNKNOWN;
        return -1;
    }

    bs->error = BSOCKET_ERROR_NONE;
    return 0;
}

int BSocket_Accept (BSocket *bs, BSocket *newsock, BAddr *addr)
{
    struct sys_addr sysaddr;
    sysaddr.len = sizeof(sysaddr.addr);
    
    int fd = accept(bs->socket, &sysaddr.addr.generic, &sysaddr.len);
    if (fd < 0) {
        #ifdef BADVPN_USE_WINAPI
        switch (WSAGetLastError()) {
            case WSAEWOULDBLOCK:
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
        }
        #else
        switch (errno) {
            case EAGAIN:
            #if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
            #endif
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
        }
        #endif
        
        bs->error = BSOCKET_ERROR_UNKNOWN;
        return -1;
    }
    
    if (!newsock) {
        close_socket(fd);
    } else {
        // set nonblocking
        if (set_nonblocking(fd) != 0) {
            DEBUG("WARNING: set_nonblocking failed");
            goto fail0;
        }
        
        DEAD_INIT(newsock->dead);
        newsock->bsys = bs->bsys;
        newsock->type = bs->type;
        newsock->socket = fd;
        newsock->have_pktinfo = 0;
        newsock->error = BSOCKET_ERROR_NONE;
        init_handlers(newsock);
        newsock->waitEvents = 0;
        newsock->connecting_status = 0;
        newsock->recv_max = BSOCKET_DEFAULT_RECV_MAX;
        newsock->recv_num = 0;
    
        if (!init_event_backend(newsock)) {
            DEBUG("WARNING: init_event_backend failed");
            goto fail0;
        }
        
        // init debug object
        DebugObject_Init(&newsock->d_obj);
    }
    
    // return client addres
    if (addr) {
        addr_sys_to_socket(addr, &sysaddr);
    }
    
    bs->error = BSOCKET_ERROR_NONE;
    return 0;
    
fail0:
    close_socket(fd);
    bs->error = BSOCKET_ERROR_UNKNOWN;
    return -1;
}

int BSocket_Send (BSocket *bs, uint8_t *data, int len)
{
    ASSERT(len >= 0)
    
    #ifdef BADVPN_USE_WINAPI
    int flags = 0;
    #else
    int flags = MSG_NOSIGNAL;
    #endif
    
    int bytes = send(bs->socket, data, len, flags);
    if (bytes < 0) {
        #ifdef BADVPN_USE_WINAPI
        switch (WSAGetLastError()) {
            case WSAEWOULDBLOCK:
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
            case WSAECONNRESET:
                if (bs->type == BSOCKET_TYPE_DGRAM) {
                    bs->error = BSOCKET_ERROR_CONNECTION_REFUSED;
                } else {
                    bs->error = BSOCKET_ERROR_CONNECTION_RESET;
                }
                return -1;
        }
        #else
        switch (errno) {
            case EAGAIN:
            #if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
            #endif
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
            case ECONNREFUSED:
                bs->error = BSOCKET_ERROR_CONNECTION_REFUSED;
                return -1;
            case ECONNRESET:
                bs->error = BSOCKET_ERROR_CONNECTION_RESET;
                return -1;
        }
        #endif
        
        bs->error = BSOCKET_ERROR_UNKNOWN;
        return -1;
    }
    
    bs->error = BSOCKET_ERROR_NONE;
    return bytes;
}

int BSocket_Recv (BSocket *bs, uint8_t *data, int len)
{
    ASSERT(len >= 0)
    
    if (limit_recv(bs)) {
        bs->error = BSOCKET_ERROR_LATER;
        return -1;
    }
    
    int bytes = recv(bs->socket, data, len, 0);
    if (bytes < 0) {
        #ifdef BADVPN_USE_WINAPI
        switch (WSAGetLastError()) {
            case WSAEWOULDBLOCK:
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
            case WSAECONNRESET:
                if (bs->type == BSOCKET_TYPE_DGRAM) {
                    bs->error = BSOCKET_ERROR_CONNECTION_REFUSED;
                } else {
                    bs->error = BSOCKET_ERROR_CONNECTION_RESET;
                }
                return -1;
        }
        #else
        switch (errno) {
            case EAGAIN:
            #if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
            #endif
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
            case ECONNREFUSED:
                bs->error = BSOCKET_ERROR_CONNECTION_REFUSED;
                return -1;
            case ECONNRESET:
                bs->error = BSOCKET_ERROR_CONNECTION_RESET;
                return -1;
        }
        #endif
        
        bs->error = BSOCKET_ERROR_UNKNOWN;
        return -1;
    }
    
    bs->error = BSOCKET_ERROR_NONE;
    return bytes;
}

int BSocket_SendTo (BSocket *bs, uint8_t *data, int len, BAddr *addr)
{
    ASSERT(len >= 0)
    ASSERT(addr)
    
    struct sys_addr remote_sysaddr;
    addr_socket_to_sys(&remote_sysaddr, addr);
    
    #ifdef BADVPN_USE_WINAPI
    int flags = 0;
    #else
    int flags = MSG_NOSIGNAL;
    #endif
    
    int bytes = sendto(bs->socket, data, len, flags, &remote_sysaddr.addr.generic, remote_sysaddr.len);
    if (bytes < 0) {
        #ifdef BADVPN_USE_WINAPI
        switch (WSAGetLastError()) {
            case WSAEWOULDBLOCK:
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
            case WSAECONNRESET:
                if (bs->type == BSOCKET_TYPE_DGRAM) {
                    bs->error = BSOCKET_ERROR_CONNECTION_REFUSED;
                } else {
                    bs->error = BSOCKET_ERROR_CONNECTION_RESET;
                }
                return -1;
        }
        #else
        switch (errno) {
            case EAGAIN:
            #if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
            #endif
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
            case ECONNREFUSED:
                bs->error = BSOCKET_ERROR_CONNECTION_REFUSED;
                return -1;
            case ECONNRESET:
                bs->error = BSOCKET_ERROR_CONNECTION_RESET;
                return -1;
        }
        #endif
        
        bs->error = BSOCKET_ERROR_UNKNOWN;
        return -1;
    }

    bs->error = BSOCKET_ERROR_NONE;
    return bytes;
}

int BSocket_RecvFrom (BSocket *bs, uint8_t *data, int len, BAddr *addr)
{
    ASSERT(len >= 0)
    ASSERT(addr)
    
    if (limit_recv(bs)) {
        bs->error = BSOCKET_ERROR_LATER;
        return -1;
    }
    
    struct sys_addr remote_sysaddr;
    remote_sysaddr.len = sizeof(remote_sysaddr.addr);
    
    int bytes = recvfrom(bs->socket, data, len, 0, &remote_sysaddr.addr.generic, &remote_sysaddr.len);
    if (bytes < 0) {
        #ifdef BADVPN_USE_WINAPI
        switch (WSAGetLastError()) {
            case WSAEWOULDBLOCK:
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
            case WSAECONNRESET:
                if (bs->type == BSOCKET_TYPE_DGRAM) {
                    bs->error = BSOCKET_ERROR_CONNECTION_REFUSED;
                } else {
                    bs->error = BSOCKET_ERROR_CONNECTION_RESET;
                }
                return -1;
        }
        #else
        switch (errno) {
            case EAGAIN:
            #if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
            #endif
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
            case ECONNREFUSED:
                bs->error = BSOCKET_ERROR_CONNECTION_REFUSED;
                return -1;
            case ECONNRESET:
                bs->error = BSOCKET_ERROR_CONNECTION_RESET;
                return -1;
        }
        #endif
        
        bs->error = BSOCKET_ERROR_UNKNOWN;
        return -1;
    }
    
    addr_sys_to_socket(addr, &remote_sysaddr);
    
    bs->error = BSOCKET_ERROR_NONE;
    return bytes;
}

int BSocket_SendToFrom (BSocket *bs, uint8_t *data, int len, BAddr *addr, BIPAddr *local_addr)
{
    ASSERT(len >= 0)
    ASSERT(addr)
    ASSERT(local_addr)
    
    if (!bs->have_pktinfo) {
        return BSocket_SendTo(bs, data, len, addr);
    }
    
    #ifdef BADVPN_USE_WINAPI
    
    // obtain WSASendMsg
    GUID guid = WSAID_WSASENDMSG;
    LPFN_WSASENDMSG WSASendMsg;
    DWORD out_bytes;
    if (WSAIoctl(bs->socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &WSASendMsg, sizeof(WSASendMsg), &out_bytes, NULL, NULL) != 0) {
        return BSocket_SendTo(bs, data, len, addr);
    }
    
    #endif
    
    struct sys_addr remote_sysaddr;
    addr_socket_to_sys(&remote_sysaddr, addr);
    
    #ifdef BADVPN_USE_WINAPI
    
    WSABUF buf;
    buf.len = len;
    buf.buf = data;
    
    union {
        char in[WSA_CMSG_SPACE(sizeof(struct in_pktinfo))];
        char in6[WSA_CMSG_SPACE(sizeof(struct in6_pktinfo))];
    } cdata;
    
    WSAMSG msg;
    memset(&msg, 0, sizeof(msg));
    msg.name = &remote_sysaddr.addr.generic;
    msg.namelen = remote_sysaddr.len;
    msg.lpBuffers = &buf;
    msg.dwBufferCount = 1;
    msg.Control.buf = (char *)&cdata;
    msg.Control.len = sizeof(cdata);
    
    int sum = 0;
    
    WSACMSGHDR *cmsg = WSA_CMSG_FIRSTHDR(&msg);
    
    switch (local_addr->type) {
        case BADDR_TYPE_NONE:
            break;
        case BADDR_TYPE_IPV4: {
            memset(cmsg, 0, WSA_CMSG_SPACE(sizeof(struct in_pktinfo)));
            cmsg->cmsg_level = IPPROTO_IP;
            cmsg->cmsg_type = IP_PKTINFO;
            cmsg->cmsg_len = WSA_CMSG_LEN(sizeof(struct in_pktinfo));
            struct in_pktinfo *pktinfo = (struct in_pktinfo *)WSA_CMSG_DATA(cmsg);
            pktinfo->ipi_addr.s_addr = local_addr->ipv4;
            sum += WSA_CMSG_SPACE(sizeof(struct in_pktinfo));
        } break;
        case BADDR_TYPE_IPV6: {
            memset(cmsg, 0, WSA_CMSG_SPACE(sizeof(struct in6_pktinfo)));
            cmsg->cmsg_level = IPPROTO_IPV6;
            cmsg->cmsg_type = IPV6_PKTINFO;
            cmsg->cmsg_len = WSA_CMSG_LEN(sizeof(struct in6_pktinfo));
            struct in6_pktinfo *pktinfo = (struct in6_pktinfo *)WSA_CMSG_DATA(cmsg);
            memcpy(pktinfo->ipi6_addr.s6_addr, local_addr->ipv6, 16);
            sum += WSA_CMSG_SPACE(sizeof(struct in6_pktinfo));
        } break;
        default:
            ASSERT(0);
    }
    
    msg.Control.len = sum;
    
    DWORD bytes;
    if (WSASendMsg(bs->socket, &msg, 0, &bytes, NULL, NULL) != 0) {
        switch (WSAGetLastError()) {
            case WSAEWOULDBLOCK:
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
            case WSAECONNRESET:
                if (bs->type == BSOCKET_TYPE_DGRAM) {
                    bs->error = BSOCKET_ERROR_CONNECTION_REFUSED;
                } else {
                    bs->error = BSOCKET_ERROR_CONNECTION_RESET;
                }
                return -1;
            default:
                bs->error = BSOCKET_ERROR_UNKNOWN;
                return -1;
        }
    }
    
    #else
    
    struct iovec iov;
    iov.iov_base = data;
    iov.iov_len = len;
    
    union {
        char in[CMSG_SPACE(sizeof(struct in_pktinfo))];
        char in6[CMSG_SPACE(sizeof(struct in6_pktinfo))];
    } cdata;
    
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &remote_sysaddr.addr.generic;
    msg.msg_namelen = remote_sysaddr.len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = &cdata;
    msg.msg_controllen = sizeof(cdata);
    
    int sum = 0;
    
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    
    switch (local_addr->type) {
        case BADDR_TYPE_NONE:
            break;
        case BADDR_TYPE_IPV4: {
            memset(cmsg, 0, CMSG_SPACE(sizeof(struct in_pktinfo)));
            cmsg->cmsg_level = IPPROTO_IP;
            cmsg->cmsg_type = IP_PKTINFO;
            cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
            struct in_pktinfo *pktinfo = (struct in_pktinfo *)CMSG_DATA(cmsg);
            pktinfo->ipi_spec_dst.s_addr = local_addr->ipv4;
            sum += CMSG_SPACE(sizeof(struct in_pktinfo));
        } break;
        case BADDR_TYPE_IPV6: {
            memset(cmsg, 0, CMSG_SPACE(sizeof(struct in6_pktinfo)));
            cmsg->cmsg_level = IPPROTO_IPV6;
            cmsg->cmsg_type = IPV6_PKTINFO;
            cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
            struct in6_pktinfo *pktinfo = (struct in6_pktinfo *)CMSG_DATA(cmsg);
            memcpy(pktinfo->ipi6_addr.s6_addr, local_addr->ipv6, 16);
            sum += CMSG_SPACE(sizeof(struct in6_pktinfo));
        } break;
        default:
            ASSERT(0);
    }
    
    msg.msg_controllen = sum;
    
    int bytes = sendmsg(bs->socket, &msg, MSG_NOSIGNAL);
    if (bytes < 0) {
        switch (errno) {
            case EAGAIN:
            #if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
            #endif
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
            case ECONNREFUSED:
                bs->error = BSOCKET_ERROR_CONNECTION_REFUSED;
                return -1;
            case ECONNRESET:
                bs->error = BSOCKET_ERROR_CONNECTION_RESET;
                return -1;
            default:
                bs->error = BSOCKET_ERROR_UNKNOWN;
                return -1;
        }
    }
    
    #endif
    
    bs->error = BSOCKET_ERROR_NONE;
    return bytes;
}

static int recvfromto_fallback (BSocket *bs, uint8_t *data, int len, BAddr *addr, BIPAddr *local_addr)
{
    int res = BSocket_RecvFrom(bs, data, len, addr);
    if (res >= 0) {
        BIPAddr_InitInvalid(local_addr);
    }
    return res;
}

int BSocket_RecvFromTo (BSocket *bs, uint8_t *data, int len, BAddr *addr, BIPAddr *local_addr)
{
    ASSERT(len >= 0)
    ASSERT(addr)
    ASSERT(local_addr)
    
    if (!bs->have_pktinfo) {
        return recvfromto_fallback(bs, data, len, addr, local_addr);
    }
    
    #ifdef BADVPN_USE_WINAPI
    
    // obtain WSARecvMsg
    GUID guid = WSAID_WSARECVMSG;
    LPFN_WSARECVMSG WSARecvMsg;
    DWORD out_bytes;
    if (WSAIoctl(bs->socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &WSARecvMsg, sizeof(WSARecvMsg), &out_bytes, NULL, NULL) != 0) {
        return recvfromto_fallback(bs, data, len, addr, local_addr);
    }
    
    #endif
    
    if (limit_recv(bs)) {
        bs->error = BSOCKET_ERROR_LATER;
        return -1;
    }
    
    struct sys_addr remote_sysaddr;
    remote_sysaddr.len = sizeof(remote_sysaddr.addr);
    
    #ifdef BADVPN_USE_WINAPI
    
    WSABUF buf;
    buf.len = len;
    buf.buf = data;
    
    union {
        char in[WSA_CMSG_SPACE(sizeof(struct in_pktinfo))];
        char in6[WSA_CMSG_SPACE(sizeof(struct in6_pktinfo))];
    } cdata;
    
    WSAMSG msg;
    memset(&msg, 0, sizeof(msg));
    msg.name = &remote_sysaddr.addr.generic;
    msg.namelen = remote_sysaddr.len;
    msg.lpBuffers = &buf;
    msg.dwBufferCount = 1;
    msg.Control.buf = (char *)&cdata;
    msg.Control.len = sizeof(cdata);
    
    DWORD bytes;
    if (WSARecvMsg(bs->socket, &msg, &bytes, NULL, NULL) != 0) {
        switch (WSAGetLastError()) {
            case WSAEWOULDBLOCK:
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
            case WSAECONNRESET:
                if (bs->type == BSOCKET_TYPE_DGRAM) {
                    bs->error = BSOCKET_ERROR_CONNECTION_REFUSED;
                } else {
                    bs->error = BSOCKET_ERROR_CONNECTION_RESET;
                }
                return -1;
            default:
                bs->error = BSOCKET_ERROR_UNKNOWN;
                return -1;
        }
    }
    
    remote_sysaddr.len = msg.namelen;
    
    #else
    
    struct iovec iov;
    iov.iov_base = data;
    iov.iov_len = len;
    
    union {
        char in[CMSG_SPACE(sizeof(struct in_pktinfo))];
        char in6[CMSG_SPACE(sizeof(struct in6_pktinfo))];
    } cdata;
    
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &remote_sysaddr.addr.generic;
    msg.msg_namelen = remote_sysaddr.len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = &cdata;
    msg.msg_controllen = sizeof(cdata);
    
    int bytes = recvmsg(bs->socket, &msg, 0);
    if (bytes < 0) {
        switch (errno) {
            case EAGAIN:
            #if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
            #endif
                bs->error = BSOCKET_ERROR_LATER;
                return -1;
            case ECONNREFUSED:
                bs->error = BSOCKET_ERROR_CONNECTION_REFUSED;
                return -1;
            case ECONNRESET:
                bs->error = BSOCKET_ERROR_CONNECTION_RESET;
                return -1;
            default:
                bs->error = BSOCKET_ERROR_UNKNOWN;
                return -1;
        }
    }
    
    remote_sysaddr.len = msg.msg_namelen;
    
    #endif
    
    addr_sys_to_socket(addr, &remote_sysaddr);
    BIPAddr_InitInvalid(local_addr);
    
    #ifdef BADVPN_USE_WINAPI
    
    WSACMSGHDR *cmsg;
    for (cmsg = WSA_CMSG_FIRSTHDR(&msg); cmsg; cmsg = WSA_CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
            struct in_pktinfo *pktinfo = (struct in_pktinfo *)WSA_CMSG_DATA(cmsg);
            BIPAddr_InitIPv4(local_addr, pktinfo->ipi_addr.s_addr);
        }
        else if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
            struct in6_pktinfo *pktinfo = (struct in6_pktinfo *)WSA_CMSG_DATA(cmsg);
            BIPAddr_InitIPv6(local_addr, pktinfo->ipi6_addr.s6_addr);
        }
    }
    
    #else
    
    struct cmsghdr *cmsg;
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
            struct in_pktinfo *pktinfo = (struct in_pktinfo *)CMSG_DATA(cmsg);
            BIPAddr_InitIPv4(local_addr, pktinfo->ipi_addr.s_addr);
        }
        else if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
            struct in6_pktinfo *pktinfo = (struct in6_pktinfo *)CMSG_DATA(cmsg);
            BIPAddr_InitIPv6(local_addr, pktinfo->ipi6_addr.s6_addr);
        }
    }
    
    #endif
    
    bs->error = BSOCKET_ERROR_NONE;
    return bytes;
}

int BSocket_GetPeerName (BSocket *bs, BAddr *addr)
{
    ASSERT(addr)
    
    struct sys_addr sysaddr;
    sysaddr.len = sizeof(sysaddr.addr);
    
    if (getpeername(bs->socket, &sysaddr.addr.generic, &sysaddr.len) < 0) {
        bs->error = BSOCKET_ERROR_UNKNOWN;
        return -1;
    }
    
    addr_sys_to_socket(addr, &sysaddr);
    
    bs->error = BSOCKET_ERROR_NONE;
    return 0;
}
