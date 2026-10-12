#ifndef HANDLERS_H
#define HANDLERS_H

struct request {
    const char* method;
    const char* path;
};

void handle_status(struct request* req);
void handle_submit_job(struct request* req);
void handle_list_jobs(struct request* req);
void handle_health(struct request* req);
void handle_metrics(struct request* req);
void log_request(struct request* req);

#endif
