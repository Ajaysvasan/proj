#include "handlers.h"

#include <stdio.h>

static void reply(struct request* req, const char* body) { printf("%s %s\n", req->path, body); }

void handle_status(struct request* req) { reply(req, "up"); }
void handle_submit_job(struct request* req) { reply(req, "queued"); }
void handle_list_jobs(struct request* req) { reply(req, "[]"); }
void handle_health(struct request* req) { reply(req, "ok"); }
void handle_metrics(struct request* req) { reply(req, "0"); }
void log_request(struct request* req) { printf("%s\n", req->path); }
