/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * NB. this file is compiled both standalone, and as part of the LOOLBroker.
 */

#include <sys/prctl.h>
#include <sys/poll.h>
#include <sys/syscall.h>
#include <signal.h>

#include <atomic>
#include <memory>
#include <iostream>

#include <Poco/Net/WebSocket.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Thread.h>
#include <Poco/ThreadPool.h>
#include <Poco/Runnable.h>
#include <Poco/StringTokenizer.h>
#include <Poco/Exception.h>
#include <Poco/Process.h>
#include <Poco/Environment.h>
#include <Poco/NotificationQueue.h>
#include <Poco/Notification.h>
#include <Poco/Mutex.h>
#include <Poco/Util/ServerApplication.h>

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKitInit.h>

#include "Common.hpp"
#include "QueueHandler.hpp"
#include "Util.hpp"
#include "ChildProcessSession.hpp"
#include "LOOLProtocol.hpp"

using namespace LOOLProtocol;
using Poco::Net::WebSocket;
using Poco::Net::HTTPClientSession;
using Poco::Net::HTTPRequest;
using Poco::Net::HTTPResponse;
using Poco::Thread;
using Poco::Runnable;
using Poco::StringTokenizer;
using Poco::Exception;
using Poco::Process;
using Poco::Notification;
using Poco::NotificationQueue;
using Poco::FastMutex;
using Poco::Util::Application;

const std::string CHILD_URI = "/loolws/child/";
const std::string LOKIT_BROKER = "/tmp/loolbroker.fifo";

class Connection: public Runnable
{
public:
    Connection(std::shared_ptr<ChildProcessSession> session,
               std::shared_ptr<WebSocket> ws) :
        _session(session),
        _ws(ws),
        _stop(false)
    {
        Log::info("Connection ctor in child for " + _session->getId());
    }

    ~Connection()
    {
        Log::info("~Connection dtor in child for " + _session->getId());
        stop();
    }

    std::shared_ptr<WebSocket> getWebSocket() const { return _ws; }
    std::shared_ptr<ChildProcessSession> getSession() { return _session; }

    void start()
    {
        _thread.start(*this);
    }

    bool isRunning()
    {
        return _thread.isRunning();
    }

    void stop()
    {
        _stop = true;
    }

    void join()
    {
        _thread.join();
    }

    void handle(TileQueue& queue, const std::string& firstLine, char* buffer, int n)
    {
        if (firstLine.find("paste") != 0)
        {
            // Everything else is expected to be a single line.
            assert(firstLine.size() == static_cast<std::string::size_type>(n));
            queue.put(firstLine);
        }
        else
            queue.put(std::string(buffer, n));
    }

    void run() override
    {
        static const std::string thread_name = "kit_ws_" + _session->getId();
#ifdef __linux
        if (prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(thread_name.c_str()), 0, 0, 0) != 0)
            Log::error("Cannot set thread name to " + thread_name + ".");
#endif
        Log::debug("Thread [" + thread_name + "] started.");

        try
        {
            TileQueue queue;
            QueueHandler handler(queue, _session, "kit_queue_" + _session->getId());

            Thread queueHandlerThread;
            queueHandlerThread.start(handler);

            int flags;
            int n;
            do
            {
                char buffer[1024];
                n = _ws->receiveFrame(buffer, sizeof(buffer), flags);
                if (n > 0)
                {
                    std::string firstLine = getFirstLine(buffer, n);
                    if (firstLine == "eof")
                    {
                        Log::info("Received EOF. Finishing.");
                        break;
                    }

                    StringTokenizer tokens(firstLine, " ", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);

                    if (firstLine == "disconnect")
                    {
                        Log::info("Client disconnected [" + (tokens.count() == 2 ? tokens[1] : std::string("no reason")) + "].");
                        break;
                    }

                    // Check if it is a "nextmessage:" and in that case read the large
                    // follow-up message separately, and handle that only.
                    int size;
                    if (tokens.count() == 2 && tokens[0] == "nextmessage:" && getTokenInteger(tokens[1], "size", size) && size > 0)
                    {
                        char largeBuffer[size];
                        n = _ws->receiveFrame(largeBuffer, size, flags);
                        if (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)
                        {
                            firstLine = getFirstLine(largeBuffer, n);
                            handle(queue, firstLine, largeBuffer, n);
                        }
                    }
                    else
                        handle(queue, firstLine, buffer, n);
                }
            }
            while (!_stop && n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE);
            Log::debug() << "Finishing " << thread_name << ". stop " << _stop
                         << ", payload size: " << n
                         << ", flags: " << std::hex << flags << Log::end;

            queue.clear();
            queue.put("eof");
            queueHandlerThread.join();

            _session->disconnect();
        }
        catch (const Exception& exc)
        {
            Log::error() << "Error: " << exc.displayText()
                         << (exc.nested() ? " (" + exc.nested()->displayText() + ")" : "")
                         << Log::end;
        }
        catch (const std::exception& exc)
        {
            Log::error(std::string("Exception: ") + exc.what());
        }
        catch (...)
        {
            Log::error("Unexpected Exception.");
        }

        Log::debug("Thread [" + thread_name + "] finished.");
    }

private:
    Thread _thread;
    std::shared_ptr<ChildProcessSession> _session;
    std::shared_ptr<WebSocket> _ws;
    volatile bool _stop;
};

/// A document container.
/// Owns LOKitDocument instance and connections.
/// Manages the lifetime of a document.
/// Technically, we can host multiple documents
/// per process. But for security reasons don't.
/// However, we could have a loolkit instance
/// per user or group of users (a trusted circle).
class Document
{
public:
    Document(LibreOfficeKit *loKit,
             const std::string& jailId,
             const std::string& url)
      : _multiView(getenv("LOK_VIEW_CALLBACK")),
        _loKit(loKit),
        _jailId(jailId),
        _url(url),
        _loKitDocument(nullptr),
        _clientViews(0)
    {
        Log::info("Document ctor for url [" + _url + "] on child [" + _jailId +
                  "] LOK_VIEW_CALLBACK=" + std::to_string(_multiView) + ".");
    }

    ~Document()
    {
        std::unique_lock<std::recursive_mutex> lock(_mutex);

        Log::info("~Document dtor for url [" + _url + "] on child [" + _jailId +
                  "]. There are " + std::to_string(_clientViews) + " views.");

        // Flag all connections to stop.
        for (auto aIterator : _connections)
        {
            aIterator.second->stop();
        }

        // Destroy all connections and views.
        for (auto aIterator : _connections)
        {
            if (TerminationState == LOOLState::LOOL_ABNORMAL)
            {
                // stop all websockets
                std::shared_ptr<WebSocket> ws = aIterator.second->getWebSocket();
                if ( ws )
                    ws->shutdownReceive();
            }
            else
            {
                // wait until loolwsd close all websockets
                aIterator.second->join();
            }
        }

        // Destroy all connections and views.
        _connections.clear();

        // TODO. check what is happening when destroying lokit document
        // Destroy the document.
        if (_loKitDocument != nullptr)
        {
            _loKitDocument->pClass->destroy(_loKitDocument);
        }
    }

    void createSession(const std::string& sessionId, const unsigned intSessionId)
    {
        std::unique_lock<std::recursive_mutex> lock(_mutex);

        const auto& it = _connections.find(intSessionId);
        if (it != _connections.end())
        {
            // found item, check if still running
            if (it->second->isRunning())
            {
                Log::warn("Thread [" + sessionId + "] is already running.");
                return;
            }

            // Restore thread.
            Log::warn("Thread [" + sessionId + "] is not running. Restoring.");
            _connections.erase(intSessionId);
        }

        Log::info() << "Creating " << (_clientViews ? "new" : "first")
                    << " view for url: " << _url << "for thread: " << sessionId
                    << " on child: " << _jailId << Log::end;

        // Open websocket connection between the child process and the
        // parent. The parent forwards us requests that it can't handle.

        HTTPClientSession cs("127.0.0.1", MASTER_PORT_NUMBER);
        cs.setTimeout(0);
        HTTPRequest request(HTTPRequest::HTTP_GET, CHILD_URI + sessionId);
        HTTPResponse response;

        auto ws = std::make_shared<WebSocket>(cs, request, response);
        ws->setReceiveTimeout(0);

        auto session = std::make_shared<ChildProcessSession>(sessionId, ws, _loKit, _loKitDocument, _jailId,
                            [this](const std::string& id, const std::string& uri) { return onLoad(id, uri); },
                            [this](const std::string& id) { onUnload(id); });
        // child Jail TID PID
        std::string hello("child " + _jailId + " " +
                          sessionId + " " + std::to_string(Process::id()));
        session->sendTextFrame(hello);

        auto thread = std::make_shared<Connection>(session, ws);
        const auto aInserted = _connections.emplace(intSessionId, thread);

        if ( aInserted.second )
            thread->start();
        else
            Log::error("Connection already exists for child: " + _jailId + ", thread: " + sessionId);

        Log::debug("Connections: " + std::to_string(_connections.size()));
    }

    /// Purges dead connections and returns
    /// the remaining number of clients.
    size_t purgeSessions()
    {
        std::vector<std::shared_ptr<ChildProcessSession>> deadSessions;
        {
            std::unique_lock<std::recursive_mutex> lock(_mutex);

            for (auto it =_connections.cbegin(); it != _connections.cend(); )
            {
                if (!it->second->isRunning())
                {
                    deadSessions.push_back(it->second->getSession());
                    it = _connections.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // Don't destroy sessions while holding our lock.
        // We may deadlock if a session is waiting on us
        // during callback initiated while handling a command
        // and the dtor tries to take its lock (which is taken).
        deadSessions.clear();

        std::unique_lock<std::recursive_mutex> lock(_mutex);
        return _connections.size();
    }

    /// Returns true if at least one *live* connection exists.
    /// Does not consider user activity, just socket status.
    bool hasConnections()
    {
        return purgeSessions() > 0;
    }

    /// Returns true if there is no activity and
    /// the document is saved.
    bool canDiscard()
    {
        //TODO: Implement proper time-out on inactivity.
        return !hasConnections();
    }

private:

    static void ViewCallback(int , const char* , void* )
    {
        //TODO: Delegate the callback.
    }

    static void DocumentCallback(int nType, const char* pPayload, void* pData)
    {
        Document* self = reinterpret_cast<Document*>(pData);
        if (self)
        {
            std::unique_lock<std::recursive_mutex> lock(self->_mutex);

            for (auto& it: self->_connections)
            {
                if (it.second->isRunning())
                {
                    auto session = it.second->getSession();
                    if (session)
                    {
                        session->loKitCallback(nType, pPayload);
                    }
                }
            }
        }
    }

    /// Load a document (or view) and register callbacks.
    LibreOfficeKitDocument* onLoad(const std::string& sessionId, const std::string& uri)
    {
        Log::info("Session " + sessionId + " is loading. " + std::to_string(_clientViews) + " views loaded.");
        const unsigned intSessionId = Util::decodeId(sessionId);

        std::unique_lock<std::recursive_mutex> lock(_mutex);

        const auto it = _connections.find(intSessionId);
        if (it == _connections.end() || !it->second)
        {
            Log::error("Cannot find session [" + sessionId + "] which decoded to " + std::to_string(intSessionId));
            return nullptr;
        }

        if (_loKitDocument == nullptr)
        {
            Log::info("Loading new document from URI: [" + uri + "] for session [" + sessionId + "].");

            if ( LIBREOFFICEKIT_HAS(_loKit, registerCallback))
                _loKit->pClass->registerCallback(_loKit, DocumentCallback, this);

            // documentLoad will trigger callback, which needs to take the lock.
            lock.unlock();
            if ((_loKitDocument = _loKit->pClass->documentLoad(_loKit, uri.c_str())) == nullptr)
            {
                Log::error("Failed to load: " + uri + ", error: " + _loKit->pClass->getError(_loKit));
                return nullptr;
            }

            // Retake the lock.
            lock.lock();
        }

        if (_multiView)
        {
            Log::info("Loading view to document from URI: [" + uri + "] for session [" + sessionId + "].");
            const auto viewId = _loKitDocument->pClass->createView(_loKitDocument);

            _loKitDocument->pClass->registerCallback(_loKitDocument, ViewCallback, reinterpret_cast<void*>(intSessionId));

            Log::info() << "Document [" << _url << "] view ["
                        << viewId << "] loaded, leaving "
                        << (_clientViews + 1) << " views." << Log::end;
        }
        else
        {
            _loKitDocument->pClass->registerCallback(_loKitDocument, DocumentCallback, this);
        }

        ++_clientViews;
        return _loKitDocument;
    }

    void onUnload(const std::string& sessionId)
    {
        std::unique_lock<std::recursive_mutex> lock(_mutex);

        const unsigned intSessionId = Util::decodeId(sessionId);
        const auto it = _connections.find(intSessionId);
        if (it == _connections.end() || !it->second)
        {
            // Nothing to do.
            return;
        }

        --_clientViews;
        Log::info("Session " + sessionId + " is unloading. " + std::to_string(_clientViews) + " views will remain.");

        if (_multiView && _loKitDocument)
        {
            Log::info() << "Document [" << _url << "] session ["
                        << sessionId << "] unloaded, leaving "
                        << _clientViews << " views." << Log::end;

            const auto viewId = _loKitDocument->pClass->getView(_loKitDocument);
            _loKitDocument->pClass->registerCallback(_loKitDocument, nullptr, nullptr);
            _loKitDocument->pClass->destroyView(_loKitDocument, viewId);
        }
    }

private:

    const bool _multiView;
    LibreOfficeKit *_loKit;
    const std::string _jailId;
    const std::string _url;

    LibreOfficeKitDocument *_loKitDocument;

    std::recursive_mutex _mutex;
    std::map<unsigned, std::shared_ptr<Connection>> _connections;
    std::atomic<unsigned> _clientViews;
};

void lokit_main(const std::string &loSubPath, const std::string& jailId, const std::string& pipe)
{
#ifdef LOOLKIT_NO_MAIN
    // Reinitialize logging when forked.
    Log::initialize("kit");
#endif

    struct pollfd aPoll;
    ssize_t nBytes = -1;
    char  aBuffer[READ_BUFFER_SIZE];
    char* pStart = nullptr;
    char* pEnd = nullptr;

    assert(!jailId.empty());
    assert(!loSubPath.empty());

    std::map<std::string, std::shared_ptr<Document>> _documents;

    static const std::string process_name = "loolkit";
#ifdef __linux
    if (prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(process_name.c_str()), 0, 0, 0) != 0)
        Log::error("Cannot set process name to " + process_name + ".");
    Util::setTerminationSignals();
    Util::setFatalSignals();
#endif
    Log::debug("Process [" + process_name + "] started.");

    static const std::string instdir_path =
#ifdef __APPLE__
                    ("/" + loSubPath + "/Frameworks");
#else
                    ("/" + loSubPath + "/program");
#endif
    LibreOfficeKit* loKit = nullptr;

    try
    {
        int writerBroker;
        int readerBroker;

        if ( (readerBroker = open(pipe.c_str(), O_RDONLY) ) < 0 )
        {
            Log::error("Error: failed to open pipe [" + pipe + "] read only.");
            exit(Application::EXIT_SOFTWARE);
        }

        if ( (writerBroker = open(LOKIT_BROKER.c_str(), O_WRONLY) ) < 0 )
        {
            Log::error("Error: failed to open pipe [" + LOKIT_BROKER + "] write only.");
            exit(Application::EXIT_SOFTWARE);
        }

        loKit = lok_init_2(instdir_path.c_str(), "file:///user");
        if (loKit == nullptr)
        {
            Log::error("Error: LibreOfficeKit initialization failed. Exiting.");
            exit(Application::EXIT_SOFTWARE);
        }

        Log::info("loolkit [" + std::to_string(Process::id()) + "] is ready.");

        std::string aResponse;
        std::string aMessage;

        while (!TerminationFlag)
        {
            if ( pStart == pEnd )
            {
                aPoll.fd = readerBroker;
                aPoll.events = POLLIN;
                aPoll.revents = 0;

                if (poll(&aPoll, 1, POLL_TIMEOUT_MS) < 0)
                {
                    Log::error("Failed to poll pipe [" + pipe + "].");
                    continue;
                }
                else
                if (aPoll.revents & (POLLIN | POLLPRI))
                {
                    nBytes = Util::readFIFO(readerBroker, aBuffer, sizeof(aBuffer));
                    if (nBytes < 0)
                    {
                        pStart = pEnd = nullptr;
                        Log::error("Error reading message from pipe [" + pipe + "].");
                        continue;
                    }
                    pStart = aBuffer;
                    pEnd   = aBuffer + nBytes;
                }
                else
                if (aPoll.revents & (POLLERR | POLLHUP))
                {
                    Log::error("Broken pipe [" + pipe + "] with broker.");
                    break;
                }
            }

            if ( pStart != pEnd )
            {
                char aChar = *pStart++;
                while (pStart != pEnd && aChar != '\r' && aChar != '\n')
                {
                    aMessage += aChar;
                    aChar = *pStart++;
                }

                if ( aChar == '\r' && *pStart == '\n')
                {
                    pStart++;
                    StringTokenizer tokens(aMessage, " ", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);
                    aResponse = std::to_string(Process::id()) + " ";

                    Log::trace("Recv: " + aMessage);
                    if (tokens[0] == "query" && tokens.count() > 1)
                    {
                        if (tokens[1] == "url")
                        {
                            for (auto it = _documents.cbegin(); it != _documents.cend(); )
                            {
                                it = (it->second->canDiscard() ? _documents.erase(it) : ++it);
                            }

                            if (_documents.empty())
                            {
                                aResponse += "empty \r\n";
                            }
                            else
                            {
                                // We really only support single URL hosting.
                                aResponse += _documents.cbegin()->first + "\r\n";
                            }
                        }
                    }
                    else if (tokens[0] == "thread")
                    {
                        const std::string& sessionId = tokens[1];
                        const unsigned intSessionId = Util::decodeId(sessionId);
                        const std::string& url = tokens[2];

                        Log::debug("Thread request for session [" + sessionId + "], url: [" + url + "].");
                        auto it = _documents.lower_bound(url);
                        if (it == _documents.end())
                            it = _documents.emplace_hint(it, url, std::make_shared<Document>(loKit, jailId, url));

                        it->second->createSession(sessionId, intSessionId);
                        aResponse += "ok \r\n";
                    }
                    else
                    {
                        aResponse = "bad \r\n";
                    }

                    Log::trace("KitToBroker: " + aResponse);
                    Util::writeFIFO(writerBroker, aResponse);
                    aMessage.clear();
                }
            }
        }

        close(writerBroker);
        close(readerBroker);
    }
    catch (const Exception& exc)
    {
        Log::error() << exc.name() << ": " << exc.displayText()
                     << (exc.nested() ? " (" + exc.nested()->displayText() + ")" : "")
                     << Log::end;
        TerminationState = LOOLState::LOOL_ABNORMAL;
    }
    catch (const std::exception& exc)
    {
        Log::error(std::string("Exception: ") + exc.what());
        TerminationState = LOOLState::LOOL_ABNORMAL;
    }

    Log::debug("Destroying documents.");
    _documents.clear();

    // Destroy LibreOfficeKit
    Log::debug("Destroying LibreOfficeKit.");
    if (loKit)
        loKit->pClass->destroy(loKit);

    Log::info("Process [" + process_name + "] finished.");
}

#ifndef LOOLKIT_NO_MAIN

/// Simple argument parsing wrapper / helper for the above.
int main(int argc, char** argv)
{
    if (std::getenv("SLEEPFORDEBUGGER"))
    {
        std::cerr << "Sleeping " << std::getenv("SLEEPFORDEBUGGER")
                  << " seconds to attach debugger to process "
                  << Process::id() << std::endl;
        Thread::sleep(std::stoul(std::getenv("SLEEPFORDEBUGGER")) * 1000);
    }

    Log::initialize("kit");

    std::string loSubPath;
    std::string jailId;
    std::string pipe;

    for (int i = 1; i < argc; ++i)
    {
        char *cmd = argv[i];
        char *eq  = nullptr;
        if (strstr(cmd, "--losubpath=") == cmd)
        {
            eq = strchrnul(cmd, '=');
            if (*eq)
                loSubPath = std::string(++eq);
        }
        else if (strstr(cmd, "--jailid=") == cmd)
        {
            eq = strchrnul(cmd, '=');
            if (*eq)
                jailId = std::string(++eq);
        }
        else if (strstr(cmd, "--pipe=") == cmd)
        {
            eq = strchrnul(cmd, '=');
            if (*eq)
                pipe = std::string(++eq);
        }
        else if (strstr(cmd, "--clientport=") == cmd)
        {
            eq = strchrnul(cmd, '=');
            if (*eq)
                ClientPortNumber = std::stoll(std::string(++eq));
        }
    }

    if (loSubPath.empty())
    {
        Log::error("Error: --losubpath is empty");
        exit(Application::EXIT_SOFTWARE);
    }

    if (jailId.empty())
    {
        Log::error("Error: --jailid is empty");
        exit(Application::EXIT_SOFTWARE);
    }

    if ( pipe.empty() )
    {
        Log::error("Error: --pipe is empty");
        exit(Application::EXIT_SOFTWARE);
    }

    try
    {
        Poco::Environment::get("LD_BIND_NOW");
    }
    catch (const Poco::NotFoundException& exc)
    {
        Log::warn("Note: LD_BIND_NOW is not set.");
    }

    try
    {
        Poco::Environment::get("LOK_VIEW_CALLBACK");
    }
    catch (const Poco::NotFoundException& exc)
    {
        Log::warn("Note: LOK_VIEW_CALLBACK is not set.");
    }

    lokit_main(loSubPath, jailId, pipe);

    return Application::EXIT_OK;
}

#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
