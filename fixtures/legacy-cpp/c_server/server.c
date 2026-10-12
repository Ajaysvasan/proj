#include <string.h>

#include "handlers.h"

struct route {
    const char* method;
    const char* path;
    void (*handler)(struct request* req);
};

/* Table-driven dispatch, as in many legacy C servers. */
static const struct route ROUTES[] = {
    {"GET", "/status", handle_status},
    {"POST", "/jobs", handle_submit_job},
    {"GET", "/jobs/list", handle_list_jobs},
};

void dispatch(struct request* req) {
    if (strcmp(req->path, "/health") == 0) {
        handle_health(req);
    } else if (strcmp(req->path, "/metrics") == 0 && strcmp(req->method, "GET") == 0) {
        log_request(req);
        handle_metrics(req);
    }
}
