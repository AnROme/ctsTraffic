/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

// parent header
#include "ctsSocket.h"
// ctl headers
#include <ctThreadPoolTimer.hpp>
#include <ctMemoryGuard.hpp>
// project headers
#include "ctsConfig.h"
#include "ctsSocketState.h"
#include "ctsWinsockLayer.h"

namespace ctsTraffic
{
    using namespace ctl;
    using namespace std;

    // default values are assigned in the class declaration
    ctsSocket::ctsSocket(weak_ptr<ctsSocketState> _parent) : parent(move(_parent))
    {
    }

    _No_competing_thread_
        ctsSocket::~ctsSocket() noexcept
    {
        // shutdown() tears down the socket object
        this->shutdown();

        // if the IO pattern is still alive, must delete it once in the d'tor before this object goes away
        // - can't reset this in ctsSocket::shutdown since ctsSocket::shutdown can be called from the parent ctsSocketState 
        //   and there may be callbacks still running holding onto a reference to this ctsSocket object
        //   which causes the potential to AV in the io_pattern
        //   (a race-condition touching the io_pattern with deleting the io_pattern)
        this->pattern.reset();
    }

    void ctsSocket::set_socket(SOCKET _socket) noexcept
    {
        const auto lock = this->socket_cs.lock();

        ctFatalCondition(
            !!this->socket,
            L"ctsSocket::set_socket trying to set a SOCKET (%Iu) when it has already been set in this object (%Iu)",
            _socket, this->socket.get());

        this->socket.reset(_socket);
    }

    int ctsSocket::close_socket(int _error_code) noexcept
    {
        const auto lock = this->socket_cs.lock();

        int error = 0;
        if (this->socket)
        {
            if (_error_code != 0)
            {
                // always try to RST if we are closing due to an error
                // to best-effort notify the opposite endpoint
                const wsIOResult result = ctsSetLingertoRSTSocket(this->socket.get());
                error = result.error_code;
            }
            this->socket.reset();
        }
        return error;
    }

    const shared_ptr<ctThreadIocp>& ctsSocket::thread_pool()
    {
        // use the SOCKET cs to also guard creation of this TP object
        const auto lock = this->socket_cs.lock();
        // must verify a valid socket first to avoid racing destrying the iocp shared_ptr as we try to create it here
        if (this->socket && !this->tp_iocp)
        {
            this->tp_iocp = make_shared<ctThreadIocp>(this->socket.get(), ctsConfig::Settings->PTPEnvironment); // can throw
        }
        return this->tp_iocp;
    }

    void ctsSocket::print_pattern_results(unsigned long _last_error) const noexcept
    {
        if (this->pattern)
        {
            this->pattern->print_stats(
                this->local_address(),
                this->target_address());
        }
        else
        {
             // failed during socket creation, bind, or connect
            ctsConfig::PrintConnectionResults(
                _last_error);
        }
    }

    void ctsSocket::complete_state(DWORD _error_code) noexcept
    {
        const auto current_io_count = ctMemoryGuardRead(&this->io_count);
        ctFatalCondition(
            (current_io_count != 0),
            L"ctsSocket::complete_state is called with outstanding IO (%d)", current_io_count);

        DWORD recorded_error = _error_code;
        if (this->pattern)
        {
            // get the pattern's last_error
            recorded_error = this->pattern->get_last_error();
            // no longer allow any more callbacks
            this->pattern->register_callback(nullptr);
        }

        auto ref_parent(this->parent.lock());
        if (ref_parent)
        {
            ref_parent->complete_state(recorded_error);
        }
    }

    const ctSockaddr& ctsSocket::local_address() const noexcept
    {
        return this->local_sockaddr;
    }

    void ctsSocket::set_local_address(const ctSockaddr& _local) noexcept
    {
        this->local_sockaddr = _local;
    }

    const ctSockaddr& ctsSocket::target_address() const noexcept
    {
        return this->target_sockaddr;
    }

    void ctsSocket::set_target_address(const ctSockaddr& _target) noexcept
    {
        this->target_sockaddr = _target;
    }

    shared_ptr<ctsIOPattern> ctsSocket::io_pattern() const noexcept
    {
        return this->pattern;
    }

    void ctsSocket::set_io_pattern(const std::shared_ptr<ctsIOPattern>& _pattern) noexcept
    {
        this->pattern = _pattern;
        if (ctsConfig::Settings->PrePostSends == 0)
        {
            // user didn't specify a specific # of sends to pend
            // start ISB notifications (best effort)
            this->initiate_isb_notification();
        }
    }

    void ctsSocket::process_isb_notification() noexcept
    {
        // lock the socket
        const auto shared_this = shared_from_this();
        const auto socket_lock(shared_this->socket_reference());
        const auto local_socket = socket_lock.socket();
        if (local_socket != INVALID_SOCKET)
        {
            ULONG isb;
            if (0 == idealsendbacklogquery(local_socket, &isb))
            {
                PrintDebugInfo(L"\t\tctsSocket::process_isb_notification : setting ISB to %u bytes\n", isb);
                this->pattern->set_ideal_send_backlog(isb);
            }
            else
            {
                const auto gle = WSAGetLastError();
                if (gle != ERROR_OPERATION_ABORTED && gle != WSAEINTR)
                {
                    ctsConfig::PrintErrorIfFailed(L"WSAIoctl(SIO_IDEAL_SEND_BACKLOG_QUERY)", gle);
                }
            }
        }
    }

    void ctsSocket::initiate_isb_notification() noexcept
    {
        try
        {
            auto& shared_iocp = thread_pool();
            OVERLAPPED* ov = shared_iocp->new_request([weak_this_ptr = std::weak_ptr<ctsSocket>(this->shared_from_this())](OVERLAPPED* ov) noexcept {
                DWORD gle = NO_ERROR;

                auto shared_this_ptr = weak_this_ptr.lock();
                if (!shared_this_ptr)
                {
                    return;
                }

                const auto socket_lock(shared_this_ptr->socket_reference());
                const auto local_socket = socket_lock.socket();
                if (local_socket != INVALID_SOCKET)
                {
                    DWORD transferred, flags; // unneeded
                    if (!WSAGetOverlappedResult(local_socket, ov, &transferred, FALSE, &flags))
                    {
                        gle = WSAGetLastError();
                        if (gle != ERROR_OPERATION_ABORTED && gle != WSAEINTR)
                        {
                            // aborted is expected whenever the socket is closed
                            ctsConfig::PrintErrorIfFailed(L"WSAIoctl(SIO_IDEAL_SEND_BACKLOG_CHANGE)", gle);
                        }
                    }
                }
                else
                {
                    gle = WSAECANCELLED;
                }
                if (gle == NO_ERROR)
                {
                    // if the request succeeded, handle the ISB change
                    // and issue the next
                    shared_this_ptr->process_isb_notification();
                    shared_this_ptr->initiate_isb_notification();
                }
            }); // lambda for new_request

            const auto shared_this = this->shared_from_this();
            const auto socket_lock(shared_this->socket_reference());
            const auto local_socket = socket_lock.socket();
            if (local_socket != INVALID_SOCKET)
            {
                const auto error = idealsendbacklognotify(local_socket, ov, nullptr);
                if (SOCKET_ERROR == error)
                {
                    const auto gle = WSAGetLastError();
                    // expect this to be pending
                    if (gle != WSA_IO_PENDING)
                    {
                        // if the ISB notification failed, tell the TP to no longer track that IO
                        shared_iocp->cancel_request(ov);
                        if (gle != ERROR_OPERATION_ABORTED && gle != WSAEINTR)
                        {
                            ctsConfig::PrintErrorIfFailed(L"WSAIoctl(SIO_IDEAL_SEND_BACKLOG_CHANGE)", gle);
                        }
                    }
                }
            }
            else
            {
                // there wasn't a SOCKET to initiate the ISB notification, tell the TP to no longer track that IO
                shared_iocp->cancel_request(ov);
            }
        }
        catch (const exception& e)
        {
            ctsConfig::PrintException(e);
        }
    }

    long ctsSocket::increment_io() noexcept
    {
        return ctMemoryGuardIncrement(&this->io_count);
    }

    long ctsSocket::decrement_io() noexcept
    {
        const auto io_value = ctMemoryGuardDecrement(&this->io_count);
        ctFatalCondition(
            (io_value < 0),
            L"ctsSocket: io count fell below zero (%d)\n", io_value);
        return io_value;
    }

    long ctsSocket::pended_io() noexcept
    {
        return ctMemoryGuardRead(&this->io_count);
    }

    void ctsSocket::shutdown() noexcept
    {
        // close the socket to trigger IO to complete/shutdown
        this->close_socket();
        // Must destroy these threadpool objects outside the CS to prevent a deadlock
        // - from when worker threads attempt to callback this ctsSocket object when IO completes
        // Must wait for the threadpool from this method when ctsSocketState calls ctsSocket::shutdown
        // - instead of calling this from the d'tor of ctsSocket, as the final reference
        //   to this ctsSocket might be from a TP thread - in which case this d'tor will deadlock
        //   (it will wait for all TP threads to exit, but it is using/blocking on of those TP threads)
        this->tp_iocp.reset();
        this->tp_timer.reset();
    }

    ///
    /// SetTimer schedules the callback function to be invoked with the given ctsSocket and ctsIOTask
    /// - note that the timer 
    /// - can throw under low resource conditions
    ///
    void ctsSocket::set_timer(const ctsIOTask& _task, function<void(weak_ptr<ctsSocket>, const ctsIOTask&)> _func)
    {
        const auto lock = this->socket_cs.lock();
        if (!this->tp_timer)
        {
            this->tp_timer = make_shared<ctThreadpoolTimer>(ctsConfig::Settings->PTPEnvironment);
        }

        // register a weak pointer after creating a shared_ptr from the 'this' ptry
        this->tp_timer->schedule_singleton(
            [_func = std::move(_func), weak_reference = this->shared_from_this(), _task]() { _func(weak_reference, _task); },
            _task.time_offset_milliseconds);
    }

} // namespace
