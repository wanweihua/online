/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <Poco/FileStream.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Process.h>
#include <Poco/URI.h>
#include <Poco/URIStreamOpener.h>

#include "Common.hpp"
#include "LOOLProtocol.hpp"
#include "LOOLSession.hpp"
#include "LOOLWSD.hpp"
#include "MasterProcessSession.hpp"
#include "Util.hpp"
#include "Rectangle.hpp"

using namespace LOOLProtocol;

using Poco::Net::SocketAddress;
using Poco::Net::WebSocket;
using Poco::Path;
using Poco::Process;
using Poco::StringTokenizer;
using Poco::UInt64;

std::map<std::string, std::shared_ptr<MasterProcessSession>> MasterProcessSession::AvailableChildSessions;
std::mutex MasterProcessSession::AvailableChildSessionMutex;
std::condition_variable MasterProcessSession::AvailableChildSessionCV;

MasterProcessSession::MasterProcessSession(const std::string& id,
                                           const Kind kind,
                                           std::shared_ptr<Poco::Net::WebSocket> ws) :
    LOOLSession(id, kind, ws),
    _pidChild(0),
    _curPart(0),
    _loadPart(-1)
{
    Log::info("MasterProcessSession ctor [" + getName() + "].");
}

MasterProcessSession::~MasterProcessSession()
{
    Log::info("~MasterProcessSession dtor [" + getName() + "].");

    try
    {
        // We could be unwinding because our peer's connection
        // died. Handle I/O errors in that case.
        disconnect();
    }
    catch (const std::exception& exc)
    {
        Log::error(std::string("Exception: ") + exc.what());
    }
}

void MasterProcessSession::disconnect(const std::string& reason)
{
    if (!isDisconnected())
    {
        LOOLSession::disconnect(reason);

        // Release the save-as queue.
        _saveAsQueue.put("");

        auto peer = _peer.lock();
        if (peer)
        {
            peer->disconnect(reason);
        }
    }
}

bool MasterProcessSession::handleDisconnect(Poco::StringTokenizer& tokens)
{
    Log::info("Graceful disconnect on " + getName() + " [" +
              (tokens.count() > 1 ? tokens[1] : std::string("no reason")) +
              "].");

    LOOLSession::handleDisconnect(tokens);

    auto peer = _peer.lock();
    if (peer)
    {
        const auto reason = (tokens.count() > 1 ? tokens[1] : std::string());
        peer->disconnect(reason);
    }

    return false;
}

bool MasterProcessSession::_handleInput(const char *buffer, int length)
{
    const std::string firstLine = getFirstLine(buffer, length);
    StringTokenizer tokens(firstLine, " ", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);

    if (tokens[0] == "loolclient")
    {
        const auto versionTuple = ParseVersion(tokens[1]);
        if (std::get<0>(versionTuple) != ProtocolMajorVersionNumber ||
            std::get<1>(versionTuple) != ProtocolMinorVersionNumber)
        {
            sendTextFrame("error: cmd=loolclient kind=badversion");
            return false;
        }

        sendTextFrame("loolserver " + GetProtocolVersion());
        return true;
    }

    if (haveSeparateProcess())
    {
        // Note that this handles both forwarding requests from the client to the child process, and
        // forwarding replies from the child process to the client. Or does it?

        // Snoop at some  messages and manipulate tile cache information as needed
        auto peer = _peer.lock();

        if (_kind == Kind::ToPrisoner)
        {
            if (tokens[0] == "curpart:" &&
                tokens.count() == 2 &&
                getTokenInteger(tokens[1], "part", _curPart))
            {
                return true;
            }

            if (tokens.count() == 2 && tokens[0] == "saveas:")
            {
                std::string url;
                if (!getTokenString(tokens[1], "url", url))
                    return true;

                if (peer)
                {
                    // Save as completed, inform the other (Kind::ToClient)
                    // MasterProcessSession about it.

                    const std::string filePrefix("file:///");
                    if (url.find(filePrefix) == 0)
                    {
                        // Rewrite file:// URLs, as they are visible to the outside world.
                        Path path(MasterProcessSession::getJailPath(_childId), url.substr(filePrefix.length()));
                        url = filePrefix + path.toString().substr(1);
                    }
                    peer->_saveAsQueue.put(url);
                }

                return true;
            }
        }

        if (_kind == Kind::ToPrisoner && peer && peer->_tileCache)
        {
            if (tokens[0] == "tile:")
            {
                int part, width, height, tilePosX, tilePosY, tileWidth, tileHeight;
                if (tokens.count() < 8 ||
                    !getTokenInteger(tokens[1], "part", part) ||
                    !getTokenInteger(tokens[2], "width", width) ||
                    !getTokenInteger(tokens[3], "height", height) ||
                    !getTokenInteger(tokens[4], "tileposx", tilePosX) ||
                    !getTokenInteger(tokens[5], "tileposy", tilePosY) ||
                    !getTokenInteger(tokens[6], "tilewidth", tileWidth) ||
                    !getTokenInteger(tokens[7], "tileheight", tileHeight))
                    assert(false);

                assert(firstLine.size() < static_cast<std::string::size_type>(length));
                peer->_tileCache->saveTile(part, width, height, tilePosX, tilePosY, tileWidth, tileHeight, buffer + firstLine.size() + 1, length - firstLine.size() - 1);
            }
            else if (tokens[0] == "status:")
            {
                peer->_tileCache->saveTextFile(std::string(buffer, length), "status.txt");
            }
            else if (tokens[0] == "commandvalues:")
            {
                const std::string stringMsg(buffer, length);
                const auto index = stringMsg.find_first_of("{");
                if (index != std::string::npos)
                {
                    const std::string stringJSON = stringMsg.substr(index);
                    Poco::JSON::Parser parser;
                    const auto result = parser.parse(stringJSON);
                    const auto object = result.extract<Poco::JSON::Object::Ptr>();
                    const std::string commandName = object->get("commandName").toString();
                    if (commandName.find(".uno:CharFontName") != std::string::npos ||
                        commandName.find(".uno:StyleApply") != std::string::npos)
                    {
                        // other commands should not be cached
                        peer->_tileCache->saveTextFile(stringMsg, "cmdValues" + commandName + ".txt");
                    }
                }
            }
            else if (tokens[0] == "partpagerectangles:")
            {
                if (tokens.count() > 1 && !tokens[1].empty())
                    peer->_tileCache->saveTextFile(std::string(buffer, length), "partpagerectangles.txt");
            }
            else if (tokens[0] == "invalidatecursor:")
            {
                peer->_tileCache->setEditing(true);
            }
            else if (tokens[0] == "invalidatetiles:")
            {
                // FIXME temporarily, set the editing on the 1st invalidate, TODO extend
                // the protocol so that the client can set the editing or view only.
                peer->_tileCache->setEditing(true);

                assert(firstLine.size() == static_cast<std::string::size_type>(length));
                peer->_tileCache->invalidateTiles(firstLine);
            }
            else if (tokens[0] == "renderfont:")
            {
                std::string font;
                if (tokens.count() < 2 ||
                    !getTokenString(tokens[1], "font", font))
                    assert(false);

                assert(firstLine.size() < static_cast<std::string::size_type>(length));
                peer->_tileCache->saveRendering(font, "font", buffer + firstLine.size() + 1, length - firstLine.size() - 1);
            }
        }

        forwardToPeer(buffer, length);
        return true;
    }

    if (tokens[0] == "child")
    {
        if (_kind != Kind::ToPrisoner)
        {
            sendTextFrame("error: cmd=child kind=invalid");
            return false;
        }
        if (!_peer.expired())
        {
            sendTextFrame("error: cmd=child kind=invalid");
            return false;
        }
        if (tokens.count() != 4)
        {
            sendTextFrame("error: cmd=child kind=syntax");
            return false;
        }

        const auto childId = tokens[1];
        setId(tokens[2]);
        const Process::PID pidChild = std::stoull(tokens[3]);

        std::unique_lock<std::mutex> lock(AvailableChildSessionMutex);
        AvailableChildSessions.emplace(getId(), shared_from_this());

        Log::info() << getName() << " mapped " << this << " childId=" << childId << ", id=" << getId()
                    << " into _availableChildSessions, size=" << AvailableChildSessions.size() << Log::end;

        _childId = childId;
        _pidChild = pidChild;
        lock.unlock();
        AvailableChildSessionCV.notify_one();
    }
    else if (_kind == Kind::ToPrisoner)
    {
        // Message from child process to be forwarded to client.

        // I think we should never get here
        Log::error(getName() + ": Unexpected request [" + tokens[0] + "].");
        assert(false);
    }
    else if (tokens[0] == "load")
    {
        if (_docURL != "")
        {
            sendTextFrame("error: cmd=load kind=docalreadyloaded");
            return false;
        }
        return loadDocument(buffer, length, tokens);
    }
    else if (tokens[0] != "canceltiles" &&
             tokens[0] != "clientzoom" &&
             tokens[0] != "clientvisiblearea" &&
             tokens[0] != "commandvalues" &&
             tokens[0] != "disconnect" &&
             tokens[0] != "downloadas" &&
             tokens[0] != "getchildid" &&
             tokens[0] != "gettextselection" &&
             tokens[0] != "paste" &&
             tokens[0] != "insertfile" &&
             tokens[0] != "invalidatetiles" &&
             tokens[0] != "key" &&
             tokens[0] != "mouse" &&
             tokens[0] != "partpagerectangles" &&
             tokens[0] != "renderfont" &&
             tokens[0] != "requestloksession" &&
             tokens[0] != "resetselection" &&
             tokens[0] != "saveas" &&
             tokens[0] != "selectgraphic" &&
             tokens[0] != "selecttext" &&
             tokens[0] != "setclientpart" &&
             tokens[0] != "setpage" &&
             tokens[0] != "status" &&
             tokens[0] != "tile" &&
             tokens[0] != "tilecombine" &&
             tokens[0] != "unload" &&
             tokens[0] != "uno")
    {
        sendTextFrame("error: cmd=" + tokens[0] + " kind=unknown");
        return false;
    }
    else if (_docURL == "")
    {
        sendTextFrame("error: cmd=" + tokens[0] + " kind=nodocloaded");
        return false;
    }
    else if (tokens[0] == "canceltiles")
    {
        if (!_peer.expired())
            forwardToPeer(buffer, length);
    }
    else if (tokens[0] == "commandvalues")
    {
        return getCommandValues(buffer, length, tokens);
    }
    else if (tokens[0] == "partpagerectangles")
    {
        return getPartPageRectangles(buffer, length);
    }
    else if (tokens[0] == "invalidatetiles")
    {
        return invalidateTiles(buffer, length, tokens);
    }
    else if (tokens[0] == "renderfont")
    {
        sendFontRendering(buffer, length, tokens);
    }
    else if (tokens[0] == "status")
    {
        return getStatus(buffer, length);
    }
    else if (tokens[0] == "tile")
    {
        sendTile(buffer, length, tokens);
    }
    else if (tokens[0] == "tilecombine")
    {
        sendCombinedTiles(buffer, length, tokens);
    }
    else
    {
        // All other commands are such that they always require a
        // LibreOfficeKitDocument session, i.e. need to be handled in
        // a child process.

        if (_peer.expired())
            dispatchChild();
        if (tokens[0] != "requestloksession")
        {
            forwardToPeer(buffer, length);
        }

        if ((tokens.count() > 1 && tokens[0] == "uno" && tokens[1] == ".uno:Save"))
        {
           _tileCache->documentSaved();
        }
        else if (tokens[0] == "disconnect")
        {
            // This was the last we would hear from the client on this socket.
            return handleDisconnect(tokens);
        }
    }
    return true;
}

bool MasterProcessSession::haveSeparateProcess()
{
    return !_childId.empty();
}

Poco::Path MasterProcessSession::getJailPath(const std::string& childId)
{
    return Path::forDirectory(LOOLWSD::ChildRoot + Path::separator() + childId);
}

bool MasterProcessSession::invalidateTiles(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    int part, tilePosX, tilePosY, tileWidth, tileHeight;

    if (tokens.count() != 6 ||
        !getTokenInteger(tokens[1], "part", part) ||
        !getTokenInteger(tokens[2], "tileposx", tilePosX) ||
        !getTokenInteger(tokens[3], "tileposy", tilePosY) ||
        !getTokenInteger(tokens[4], "tilewidth", tileWidth) ||
        !getTokenInteger(tokens[5], "tileheight", tileHeight))
    {
        sendTextFrame("error: cmd=invalidatetiles kind=syntax");
        return false;
    }

    // FIXME temporarily, set the editing on the 1st invalidate, TODO extend
    // the protocol so that the client can set the editing or view only.
    _tileCache->setEditing(true);

    _tileCache->invalidateTiles(_curPart, tilePosX, tilePosY, tileWidth, tileHeight);
    return true;
}

bool MasterProcessSession::loadDocument(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    if (tokens.count() < 2)
    {
        sendTextFrame("error: cmd=load kind=syntax");
        return false;
    }

    std::string timestamp;
    parseDocOptions(tokens, _loadPart, timestamp);

    try
    {
        Poco::URI aUri(_docURL);

        // request new URL session
        const std::string aMessage = "request " + getId() + " " + _docURL + "\r\n";
        Log::trace("MasterToBroker: " + aMessage);
        Util::writeFIFO(LOOLWSD::BrokerWritePipe, aMessage);
    }
    catch (const Poco::SyntaxException&)
    {
        sendTextFrame("error: cmd=load kind=uriinvalid");
        return false;
    }

    _tileCache.reset(new TileCache(_docURL, timestamp));

    // Finally, wait for the Child to connect to Master,
    // link the document in jail and dispatch load to child.
    dispatchChild();

    return true;
}

bool MasterProcessSession::getStatus(const char *buffer, int length)
{
    std::string status;

    status = _tileCache->getTextFile("status.txt");
    if (status.size() > 0)
    {
        sendTextFrame(status);
        return true;
    }

    if (_peer.expired())
        dispatchChild();
    forwardToPeer(buffer, length);
    return true;
}

bool MasterProcessSession::getCommandValues(const char *buffer, int length, StringTokenizer& tokens)
{
    std::string command;
    if (tokens.count() != 2 || !getTokenString(tokens[1], "command", command))
    {
        sendTextFrame("error: cmd=commandvalues kind=syntax");
        return false;
    }

    std::string cmdValues = _tileCache->getTextFile("cmdValues" + command + ".txt");
    if (cmdValues.size() > 0)
    {
        sendTextFrame(cmdValues);
        return true;
    }

    if (_peer.expired())
        dispatchChild();
    forwardToPeer(buffer, length);
    return true;
}

bool MasterProcessSession::getPartPageRectangles(const char *buffer, int length)
{
    std::string partPageRectangles = _tileCache->getTextFile("partpagerectangles.txt");
    if (partPageRectangles.size() > 0)
    {
        sendTextFrame(partPageRectangles);
        return true;
    }

    if (_peer.expired())
        dispatchChild();
    forwardToPeer(buffer, length);
    return true;
}

std::string MasterProcessSession::getSaveAs()
{
    return _saveAsQueue.get();
}

void MasterProcessSession::sendFontRendering(const char *buffer, int length, StringTokenizer& tokens)
{
    std::string font;

    if (tokens.count() < 2 ||
        !getTokenString(tokens[1], "font", font))
    {
        sendTextFrame("error: cmd=renderfont kind=syntax");
        return;
    }

    std::string response = "renderfont: " + Poco::cat(std::string(" "), tokens.begin() + 1, tokens.end()) + "\n";

    std::vector<char> output;
    output.resize(response.size());
    std::memcpy(output.data(), response.data(), response.size());

    std::unique_ptr<std::fstream> cachedRendering = _tileCache->lookupRendering(font, "font");
    if (cachedRendering && cachedRendering->is_open())
    {
        cachedRendering->seekg(0, std::ios_base::end);
        size_t pos = output.size();
        std::streamsize size = cachedRendering->tellg();
        output.resize(pos + size);
        cachedRendering->seekg(0, std::ios_base::beg);
        cachedRendering->read(output.data() + pos, size);
        cachedRendering->close();

        sendBinaryFrame(output.data(), output.size());
        return;
    }

    if (_peer.expired())
        dispatchChild();
    forwardToPeer(buffer, length);
}

void MasterProcessSession::sendTile(const char *buffer, int length, StringTokenizer& tokens)
{
    int part, width, height, tilePosX, tilePosY, tileWidth, tileHeight;

    if (tokens.count() < 8 ||
        !getTokenInteger(tokens[1], "part", part) ||
        !getTokenInteger(tokens[2], "width", width) ||
        !getTokenInteger(tokens[3], "height", height) ||
        !getTokenInteger(tokens[4], "tileposx", tilePosX) ||
        !getTokenInteger(tokens[5], "tileposy", tilePosY) ||
        !getTokenInteger(tokens[6], "tilewidth", tileWidth) ||
        !getTokenInteger(tokens[7], "tileheight", tileHeight))
    {
        sendTextFrame("error: cmd=tile kind=syntax");
        return;
    }

    if (part < 0 ||
        width <= 0 ||
        height <= 0 ||
        tilePosX < 0 ||
        tilePosY < 0 ||
        tileWidth <= 0 ||
        tileHeight <= 0)
    {
        sendTextFrame("error: cmd=tile kind=invalid");
        return;
    }

    std::string response = "tile: " + Poco::cat(std::string(" "), tokens.begin() + 1, tokens.end()) + "\n";

    std::vector<char> output;
    output.reserve(4 * width * height);
    output.resize(response.size());
    std::memcpy(output.data(), response.data(), response.size());

    std::unique_ptr<std::fstream> cachedTile = _tileCache->lookupTile(part, width, height, tilePosX, tilePosY, tileWidth, tileHeight);
    if (cachedTile && cachedTile->is_open())
    {
        cachedTile->seekg(0, std::ios_base::end);
        size_t pos = output.size();
        std::streamsize size = cachedTile->tellg();
        output.resize(pos + size);
        cachedTile->seekg(0, std::ios_base::beg);
        cachedTile->read(output.data() + pos, size);
        cachedTile->close();

        sendBinaryFrame(output.data(), output.size());

        return;
    }

    if (_peer.expired())
        dispatchChild();
    forwardToPeer(buffer, length);
}


void MasterProcessSession::sendCombinedTiles(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    int part, pixelWidth, pixelHeight, tileWidth, tileHeight;
    std::string tilePositionsX, tilePositionsY;

    if (tokens.count() < 8 ||
        !getTokenInteger(tokens[1], "part", part) ||
        !getTokenInteger(tokens[2], "width", pixelWidth) ||
        !getTokenInteger(tokens[3], "height", pixelHeight) ||
        !getTokenString (tokens[4], "tileposx", tilePositionsX) ||
        !getTokenString (tokens[5], "tileposy", tilePositionsY) ||
        !getTokenInteger(tokens[6], "tilewidth", tileWidth) ||
        !getTokenInteger(tokens[7], "tileheight", tileHeight))
    {
        sendTextFrame("error: cmd=tilecombine kind=syntax");
        return;
    }

    if (part < 0 || pixelWidth <= 0 || pixelHeight <= 0
       || tileWidth <= 0 || tileHeight <= 0
       || tilePositionsX.empty() || tilePositionsY.empty())
    {
        sendTextFrame("error: cmd=tilecombine kind=invalid");
        return;
    }

    Util::Rectangle renderArea;

    StringTokenizer positionXtokens(tilePositionsX, ",", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);
    StringTokenizer positionYtokens(tilePositionsY, ",", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);

    size_t numberOfPositions = positionYtokens.count();

    // check that number of positions for X and Y is the same
    if (numberOfPositions != positionYtokens.count())
    {
        sendTextFrame("error: cmd=tilecombine kind=invalid");
        return;
    }

    std::string forwardTileX;
    std::string forwardTileY;

    for (size_t i = 0; i < numberOfPositions; i++)
    {
        int x, y;

        if (!stringToInteger(positionXtokens[i], x))
        {
            sendTextFrame("error: cmd=tilecombine kind=syntax");
            return;
        }
        if (!stringToInteger(positionYtokens[i], y))
        {
            sendTextFrame("error: cmd=tilecombine kind=syntax");
            return;
        }

        std::unique_ptr<std::fstream> cachedTile = _tileCache->lookupTile(part, pixelWidth, pixelHeight, x, y, tileWidth, tileHeight);

        if (cachedTile && cachedTile->is_open())
        {
            std::string response = "tile: part=" + std::to_string(part) +
                               " width=" + std::to_string(pixelWidth) +
                               " height=" + std::to_string(pixelHeight) +
                               " tileposx=" + std::to_string(x) +
                               " tileposy=" + std::to_string(y) +
                               " tilewidth=" + std::to_string(tileWidth) +
                               " tileheight=" + std::to_string(tileHeight) + "\n";

            std::vector<char> output;
            output.reserve(4 * pixelWidth * pixelHeight);
            output.resize(response.size());
            std::memcpy(output.data(), response.data(), response.size());
            cachedTile->seekg(0, std::ios_base::end);
            size_t pos = output.size();
            std::streamsize size = cachedTile->tellg();
            output.resize(pos + size);
            cachedTile->seekg(0, std::ios_base::beg);
            cachedTile->read(output.data() + pos, size);
            cachedTile->close();

            sendBinaryFrame(output.data(), output.size());
        }
        else
        {
            if (!forwardTileX.empty())
                forwardTileX += ",";
            forwardTileX += std::to_string(x);

            if (!forwardTileY.empty())
                forwardTileY += ",";
            forwardTileY += std::to_string(y);
        }
    }

    if (forwardTileX.empty() && forwardTileY.empty())
        return;

    if (_peer.expired())
        dispatchChild();

    std::string forward = "tilecombine part=" + std::to_string(part) +
                               " width=" + std::to_string(pixelWidth) +
                               " height=" + std::to_string(pixelHeight) +
                               " tileposx=" + forwardTileX +
                               " tileposy=" + forwardTileY +
                               " tilewidth=" + std::to_string(tileWidth) +
                               " tileheight=" + std::to_string(tileHeight);

    forwardToPeer(forward.c_str(), forward.size());
}

void MasterProcessSession::dispatchChild()
{
    short nRequest = 3;
    bool  bFound = false;

    if (_bShutdown)
        return;

    // Wait until the child has connected with Master.
    std::shared_ptr<MasterProcessSession> childSession;
    std::unique_lock<std::mutex> lock(AvailableChildSessionMutex);

    Log::debug() << "Waiting for a child session permission for thread [" << getId() << "]." << Log::end;
    while (nRequest-- && !bFound)
    {
        AvailableChildSessionCV.wait_for(
            lock,
            std::chrono::milliseconds(2000),
            [&bFound, this]
            {
                return (bFound = AvailableChildSessions.find(getId()) != AvailableChildSessions.end());
            });

        if (!bFound)
        {
            Log::info() << "Retrying child permission... " << nRequest << Log::end;
            // request again new URL session
            const std::string aMessage = "request " + getId() + " " + _docURL + "\r\n";
            Log::trace("MasterToBroker: " + aMessage);
            Util::writeFIFO(LOOLWSD::BrokerWritePipe, aMessage);
        }
    }

    if (bFound)
    {
        Log::debug("Waiting child session permission, done!");
        childSession = AvailableChildSessions[getId()];
        AvailableChildSessions.erase(getId());
    }

    lock.unlock();

    if (nRequest < 0 && !bFound)
    {
        Log::error(getName() + ": Failed to connect to child. Shutting down socket.");
        Util::shutdownWebSocket(_ws);
        return;
    }

    const auto jailRoot = Poco::Path(LOOLWSD::ChildRoot, LOOLWSD::JailId);
    const auto childId = std::to_string(childSession->_pidChild);

    auto document = DocumentURI::create(_docURL, jailRoot.toString(), childId);

    _peer = childSession;
    childSession->_peer = shared_from_this();

    std::ostringstream oss;
    oss << "load";
    oss << " url=" << document->getPublicUri().toString();
    oss << " jail=" << document->getJailedUri().toString();

    if (_loadPart >= 0)
        oss << " part=" + std::to_string(_loadPart);

    if (!_docOptions.empty())
        oss << " options=" << _docOptions;

    const auto loadRequest = oss.str();
    forwardToPeer(loadRequest.c_str(), loadRequest.size());
}

void MasterProcessSession::forwardToPeer(const char *buffer, int length)
{
    const auto message = getAbbreviatedMessage(buffer, length);
    Log::trace(_kindString + ",forwardToPeer," + message);

    auto peer = _peer.lock();
    if (!peer)
    {
        Log::error(getName() + ": no peer to forward to.");
        return;
    }
    peer->sendBinaryFrame(buffer, length);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
