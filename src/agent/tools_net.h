/* AIagent98 - network tools ([net] section gates all of them). */
#ifndef A98_TOOLS_NET_H
#define A98_TOOLS_NET_H

#include "tools.h"

extern const ToolDef tool_http_request;
extern const ToolDef tool_http_download;
extern const ToolDef tool_dns_resolve;
extern const ToolDef tool_tcp_probe;
extern const ToolDef tool_web_search;
extern const ToolDef tool_gist_upload;

int tool_http_request_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);
int tool_http_download_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);
int tool_dns_resolve_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);
int tool_tcp_probe_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);
int tool_web_search_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);
int tool_gist_upload_run(struct Agent *a, cJSON *args, Str *out, char *err, size_t errcap);

extern const char schema_http_request[];
extern const char schema_http_download[];
extern const char schema_dns_resolve[];
extern const char schema_tcp_probe[];
extern const char schema_web_search[];
extern const char schema_gist_upload[];

#endif /* A98_TOOLS_NET_H */


