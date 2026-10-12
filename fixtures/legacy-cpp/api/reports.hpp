#include <string>

namespace reports {

class Request {};

class RequestHandler {
 public:
  virtual ~RequestHandler() = default;
  virtual void handleRequest(Request& req) = 0;
};

class ReportHandler : public RequestHandler {
 public:
  void handleRequest(Request& req) override;
  void render(int pageCount);
  void render(const std::string& title);
};

class ExportHandler : public RequestHandler {
 public:
  void handleRequest(Request& req) override;
};

class HandlerFactory {
 public:
  RequestHandler* createRequestHandler(const std::string& uri);
};

}  // namespace reports
