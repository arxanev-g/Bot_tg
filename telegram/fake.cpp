#include "fake.h"
#include "fake_data.h"

#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include <Poco/URI.h>

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/SocketAddress.h>

#include <Poco/JSON/Parser.h>

namespace telegram {

using namespace Poco;
using namespace Poco::Net;

class CheckFailedException : public std::exception {};

class TestCase {
public:
  virtual void HandleRequest(HTTPServerRequest &request,
                             HTTPServerResponse &response) = 0;

  std::mutex Mutex;

  std::vector<std::string> Expectations;
  int Fulfilled = 0;

  std::vector<std::string> Fails;

  void Fail(const std::string &message) {
    Fails.push_back(message);
    throw CheckFailedException();
  }

  void ExpectURI(HTTPServerRequest &request, std::string uri) {
    auto req_uri = URI(request.getURI());
    auto req_query = req_uri.getQueryParameters();
    auto compare_uri = URI(uri);
    auto compare_query = compare_uri.getQueryParameters();

    std::sort(req_query.begin(), req_query.end());
    std::sort(compare_query.begin(), compare_query.end());

    if (req_uri.getHost() != compare_uri.getHost()) {
      Fail("Invalid Host: expected " + compare_uri.getHost() + ", got " +
           req_uri.getHost());
    }

    if (req_uri.getPath() != compare_uri.getPath()) {
      Fail("Invalid Path: expected " + compare_uri.getPath() + ", got " +
           req_uri.getPath());
    }

    if (req_query != compare_query) {
      Fail("Invalid Query params");
    }
  }

  void ExpectMethod(HTTPServerRequest &request, std::string method) {
    if (request.getMethod() != method) {
      Fail("Invalid method: expected " + method + ", got " +
           request.getMethod());
    }
  }

  void Check() {
    std::stringstream errors;

    bool fail = false;

    for (size_t i = Fulfilled; i < Expectations.size(); ++i) {
      fail = true;
      errors << "Expectation not satisfied: " << Expectations[i] << std::endl;
    }

    for (auto error : Fails) {
      fail = true;
      errors << "Error encountered: " << error << std::endl;
    }

    if (fail) {
      throw std::runtime_error(errors.str());
    }
  }
};

class SingleGetMeTestCase : public TestCase {
public:
  SingleGetMeTestCase() { Expectations = {"Client sends getMe request"}; }

  void HandleRequest(HTTPServerRequest &request,
                     HTTPServerResponse &response) override {
    ExpectURI(request, "/bot123/getMe");
    ExpectMethod(request, "GET");

    Fulfilled++;
    if (Fulfilled == 1) {
      response.setStatus(HTTPResponse::HTTP_OK);
      response.send() << FakeData::GetMeJson;
    } else {
      Fail("Unexpected extra request");
    }
  }
};

class ErrorHandlingTestCase : public TestCase {
public:
  ErrorHandlingTestCase() {
    Expectations = {
        "Client sends getMe request and receives Internal Server error",
        "Client sends getMe request and receives error json"};
  }

  void HandleRequest(HTTPServerRequest &request,
                     HTTPServerResponse &response) override {
    ExpectURI(request, "/bot123/getMe");
    ExpectMethod(request, "GET");

    ++Fulfilled;
    if (Fulfilled == 1) {
      response.setStatus(HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
      response.send() << "Internal server error";
    } else if (Fulfilled == 2) {
      response.setStatus(HTTPResponse::HTTP_UNAUTHORIZED);
      response.send() << FakeData::GetMeErrorJson;
    } else {
      Fail("Unexpected extra request");
    }
  }
};

class GetUpdatesAndSendMessagesTestCase : public TestCase {
public:
  GetUpdatesAndSendMessagesTestCase() {
    Expectations = {
        "Client sends getUpdates request", "Client sends message \"Hi!\"",
        "Client sends reply \"Reply\"", "Client sends reply \"Reply\""};
  }

  void HandleRequest(HTTPServerRequest &request,
                     HTTPServerResponse &response) override {
    auto checkContentType = [&] {
      if (request.get("Content-Type") != "application/json") {
        Fail("Content-Type header is not set");
      }
    };

    int64_t chatId;
    std::string text;
    bool hasReplyToMessageId = false;
    int64_t replyToMessageId;
    auto parseJson = [&] {
      Poco::JSON::Parser parser;
      auto body = parser.parse(request.stream());
      auto message = body.extract<Poco::JSON::Object::Ptr>();

      chatId = message->getValue<int64_t>("chat_id");
      text = message->getValue<std::string>("text");

      if (message->has("reply_to_message_id")) {
        hasReplyToMessageId = true;
        replyToMessageId = message->getValue<int64_t>("reply_to_message_id");
      }
    };

    ++Fulfilled;
    if (Fulfilled == 1) {
      ExpectURI(request, "/bot123/getUpdates");
      ExpectMethod(request, "GET");

      response.setStatus(HTTPResponse::HTTP_OK);
      response.send() << FakeData::GetUpdatesFourMessagesJson;
    } else if (Fulfilled == 2) {
      ExpectURI(request, "/bot123/sendMessage");
      ExpectMethod(request, "POST");
      checkContentType();
      parseJson();

      if (text != "Hi!") {
        Fail("Invalid text in message #1");
      }

      if (chatId != 104519755) {
        Fail("Invalid chat_id in message #1");
      }

      response.setStatus(HTTPResponse::HTTP_OK);
      response.send() << FakeData::SendMessageHiJson;
    } else if (Fulfilled == 3 || Fulfilled == 4) {
      ExpectURI(request, "/bot123/sendMessage");
      ExpectMethod(request, "POST");
      checkContentType();
      parseJson();

      if (text != "Reply") {
        Fail("Invalid text in reply message");
      }

      if (chatId != 104519755) {
        Fail("Invalid chat id in reply message");
      }

      if (!hasReplyToMessageId || replyToMessageId != 2) {
        Fail("reply_to_message_id field is incorrect");
      }

      response.setStatus(HTTPResponse::HTTP_OK);
      response.send() << FakeData::SendMessageReplyJson;
    } else {
      Fail("Unexpected extra request");
    }
  }
};

class HandleOffsetTestCase : public TestCase {
public:
  HandleOffsetTestCase() {
    Expectations = {
        "Client sends request and receives 2 messages",
        "Client sends request with correct offset and receives 0 messages",
        "Client sends request with current offset and receives 1 message"};
  }

  void HandleRequest(HTTPServerRequest &request,
                     HTTPServerResponse &response) override {
    ++Fulfilled;

    if (Fulfilled == 1) {
      ExpectURI(request, "/bot123/getUpdates?timeout=5");
      ExpectMethod(request, "GET");

      response.setStatus(HTTPResponse::HTTP_OK);
      response.send() << FakeData::GetUpdatesTwoMessages;
    } else if (Fulfilled == 2) {
      ExpectURI(request, "/bot123/getUpdates?offset=851793508&timeout=5");
      ExpectMethod(request, "GET");

      response.setStatus(HTTPResponse::HTTP_OK);
      response.send() << FakeData::GetUpdatesZeroMessages;
    } else if (Fulfilled == 3) {
      ExpectURI(request, "/bot123/getUpdates?offset=851793508&timeout=5");
      ExpectMethod(request, "GET");

      response.setStatus(HTTPResponse::HTTP_OK);
      response.send() << FakeData::GetupdatesOneMessage;
    } else {
      Fail("Unexpected extra request");
    }
  }
};

class FakeHandler : public HTTPRequestHandler {
public:
  FakeHandler(TestCase *testCase) : TestCase_(testCase) {}

  virtual void handleRequest(HTTPServerRequest &request,
                             HTTPServerResponse &response) override {
    std::unique_lock<std::mutex> guard(TestCase_->Mutex);
    try {
      TestCase_->HandleRequest(request, response);
    } catch (const CheckFailedException &e) {
      response.setStatus(HTTPResponse::HTTP_BAD_REQUEST);
      response.send();
    } catch (const std::exception &e) {
      TestCase_->Fail(e.what());
      throw;
    }
  }

private:
  TestCase *TestCase_;
};

class FakeHandlerFactory : public HTTPRequestHandlerFactory {
public:
  FakeHandlerFactory(TestCase *testCase) : TestCase_(testCase) {}

  virtual HTTPRequestHandler *createRequestHandler(const HTTPServerRequest &) {
    return new FakeHandler(TestCase_);
  }

private:
  TestCase *TestCase_;
};

FakeServer::FakeServer(const std::string &testCase) {
  if (testCase == "Single getMe") {
    TestCase_.reset(new SingleGetMeTestCase());
  } else if (testCase == "getMe error handling") {
    TestCase_.reset(new ErrorHandlingTestCase());
  } else if (testCase == "Single getUpdates and send messages") {
    TestCase_.reset(new GetUpdatesAndSendMessagesTestCase());
  } else if (testCase == "Handle getUpdates offset") {
    TestCase_.reset(new HandleOffsetTestCase());
  } else {
    throw std::runtime_error("Unknown test case name " + testCase);
  }
}

FakeServer::~FakeServer() { Stop(); }

void FakeServer::Start() {
  Socket_.reset(new ServerSocket(SocketAddress("localhost", 8080)));

  Server_.reset(new HTTPServer(new FakeHandlerFactory(TestCase_.get()),
                               *Socket_, new HTTPServerParams()));

  Server_->start();
}

std::string FakeServer::GetUrl() {
  return "http://localhost:" + std::to_string(8080) + "/";
}

void FakeServer::Stop() {
  if (Server_) {
    Server_->stop();

    Server_.reset();
    Socket_.reset();
  }
}

void FakeServer::StopAndCheckExpectations() {
  Stop();

  TestCase_->Check();
}

} // namespace telegram
