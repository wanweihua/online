/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/Path.h>
#include <Poco/StringTokenizer.h>
#include <Poco/URI.h>
#include <cppunit/extensions/HelperMacros.h>

#include <LOOLProtocol.hpp>
#include <Common.hpp>
#include <ChildProcessSession.hpp>

/// Tests the HTTP WebSocket API of loolwsd. The server has to be started manually before running this test.
class HTTPWSTest : public CPPUNIT_NS::TestFixture
{
    const Poco::URI _uri;
    Poco::Net::HTTPClientSession _session;
    Poco::Net::HTTPRequest _request;
    Poco::Net::HTTPResponse _response;
    Poco::Net::WebSocket _socket;

    CPPUNIT_TEST_SUITE(HTTPWSTest);
    CPPUNIT_TEST(testPaste);
    CPPUNIT_TEST(testLargePaste);
    CPPUNIT_TEST(testRenderingOptions);
    CPPUNIT_TEST_SUITE_END();

    void testPaste();
    void testLargePaste();
    void testRenderingOptions();

    static
    void sendTextFrame(Poco::Net::WebSocket& socket, const std::string& string);

public:
    HTTPWSTest()
        : _uri("http://127.0.0.1:" + std::to_string(ClientPortNumber)),
          _session(_uri.getHost(), _uri.getPort()),
          _request(Poco::Net::HTTPRequest::HTTP_GET, "/ws"),
          _socket(_session, _request, _response)
    {
    }

    void setUp()
    {
        _socket.shutdown();
        _socket = Poco::Net::WebSocket(_session, _request, _response);
    }

    void tearDown()
    {
        _socket.shutdown();
    }
};

void HTTPWSTest::testPaste()
{
    // Load a document and make it empty.
    const std::string documentPath = TDOC "/hello.odt";
    const std::string documentURL = "file://" + Poco::Path(documentPath).makeAbsolute().toString();
    sendTextFrame(_socket, "load url=" + documentURL);
    sendTextFrame(_socket, "uno .uno:SelectAll");
    sendTextFrame(_socket, "uno .uno:Delete");

    // Paste some text into it.
    sendTextFrame(_socket, "paste mimetype=text/plain;charset=utf-8\naaa bbb ccc");

    // Check if the document contains the pasted text.
    sendTextFrame(_socket, "uno .uno:SelectAll");
    sendTextFrame(_socket, "gettextselection mimetype=text/plain;charset=utf-8");
    std::string selection;
    int flags;
    int n;
    do
    {
        char buffer[READ_BUFFER_SIZE];
        n = _socket.receiveFrame(buffer, sizeof(buffer), flags);
        if (n > 0)
        {
            const std::string line = LOOLProtocol::getFirstLine(buffer, n);
            const std::string prefix = "textselectioncontent: ";
            if (line.find(prefix) == 0)
            {
                selection = line.substr(prefix.length());
                break;
            }
        }
    }
    while (n > 0 && (flags & Poco::Net::WebSocket::FRAME_OP_BITMASK) != Poco::Net::WebSocket::FRAME_OP_CLOSE);
    sendTextFrame(_socket, "disconnect");
    _socket.shutdown();
    CPPUNIT_ASSERT_EQUAL(std::string("aaa bbb ccc"), selection);
}

void HTTPWSTest::testLargePaste()
{
    // Load a document and make it empty.
    std::string documentPath = TDOC "/hello.odt";
    std::string documentURL = "file://" + Poco::Path(documentPath).makeAbsolute().toString();
    sendTextFrame(_socket, "load url=" + documentURL);
    sendTextFrame(_socket, "uno .uno:SelectAll");
    sendTextFrame(_socket, "uno .uno:Delete");

    // Paste some text into it.
    std::ifstream documentStream(documentPath);
    std::string documentContents((std::istreambuf_iterator<char>(documentStream)), std::istreambuf_iterator<char>());
    sendTextFrame(_socket, "paste mimetype=text/html\n" + documentContents);

    // Check if the server is still alive.
    // This resulted first in a hang, as respose for the message never arrived, then a bit later in a Poco::TimeoutException.
    sendTextFrame(_socket, "gettextselection mimetype=text/plain;charset=utf-8");
    std::string selection;
    int flags;
    int n;
    do
    {
        char buffer[READ_BUFFER_SIZE];
        n = _socket.receiveFrame(buffer, sizeof(buffer), flags);
        if (n > 0)
        {
            std::string line = LOOLProtocol::getFirstLine(buffer, n);
            std::string prefix = "textselectioncontent: ";
            if (line.find(prefix) == 0)
                break;
        }
    }
    while (n > 0 && (flags & Poco::Net::WebSocket::FRAME_OP_BITMASK) != Poco::Net::WebSocket::FRAME_OP_CLOSE);
    sendTextFrame(_socket, "disconnect");
    _socket.shutdown();
}

void HTTPWSTest::testRenderingOptions()
{
    // Load a document and get its size.
    const std::string documentPath = TDOC "/hide-whitespace.odt";
    const std::string documentURL = "file://" + Poco::Path(documentPath).makeAbsolute().toString();
    const std::string options = "{\"rendering\":{\".uno:HideWhitespace\":{\"type\":\"boolean\",\"value\":\"true\"}}}";
    sendTextFrame(_socket, "load url=" + documentURL + " options=" + options);
    sendTextFrame(_socket, "status");

    std::string status;
    int flags;
    int n;
    do
    {
        char buffer[READ_BUFFER_SIZE];
        n = _socket.receiveFrame(buffer, sizeof(buffer), flags);
        if (n > 0)
        {
            std::string line = LOOLProtocol::getFirstLine(buffer, n);
            std::string prefix = "status: ";
            if (line.find(prefix) == 0)
            {
                status = line.substr(prefix.length());
                break;
            }
        }
    }
    while (n > 0 && (flags & Poco::Net::WebSocket::FRAME_OP_BITMASK) != Poco::Net::WebSocket::FRAME_OP_CLOSE);
    sendTextFrame(_socket, "disconnect");
    _socket.shutdown();
    // Expected format is something like 'type=text parts=2 current=0 width=12808 height=1142'.
    Poco::StringTokenizer tokens(status, " ", Poco::StringTokenizer::TOK_IGNORE_EMPTY | Poco::StringTokenizer::TOK_TRIM);
    CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(5), tokens.count());

    const std::string token = tokens[4];
    const std::string prefix = "height=";
    CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(0), token.find(prefix));
    const int height = std::stoi(token.substr(prefix.size()));
    // HideWhitespace was ignored, this was 32532, should be around 16706.
    CPPUNIT_ASSERT(height < 20000);
}

void HTTPWSTest::sendTextFrame(Poco::Net::WebSocket& socket, const std::string& string)
{
    socket.sendFrame(string.data(), string.size());
}

CPPUNIT_TEST_SUITE_REGISTRATION(HTTPWSTest);

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
