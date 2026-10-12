#include "reports.hpp"

namespace reports {

void ReportHandler::handleRequest(Request& req) {
  render("Monthly report");
  render(12);
}

void ReportHandler::render(int pageCount) {}
void ReportHandler::render(const std::string& title) {}

void ExportHandler::handleRequest(Request& req) {}

// Poco-style factory: the URI picks the handler class.
RequestHandler* HandlerFactory::createRequestHandler(const std::string& uri) {
  if (uri == "/reports") return new ReportHandler();
  if (uri == "/reports/export") return new ExportHandler();
  return nullptr;
}

}  // namespace reports
