#ifndef _MSRP_H
#define _MSRP_H

#include <switch.h>

enum {
	MSRP_ST_WAIT_HEADER,
	MSRP_ST_PARSE_HEADER,
	MSRP_ST_WAIT_BODY,
	MSRP_ST_DONE,
	MSRP_ST_ERROR,

	MSRP_METHOD_REPLY,
	MSRP_METHOD_SEND,
	MSRP_METHOD_AUTH,
	MSRP_METHOD_REPORT,

};

enum {
	MSRP_H_FROM_PATH,
	MSRP_H_TO_PATH,
	MSRP_H_MESSAGE_ID,
	MSRP_H_CONTENT_TYPE,
	MSRP_H_SUCCESS_REPORT,
	MSRP_H_FAILURE_REPORT,
	MSRP_H_UNKNOWN
};

typedef struct msrp_msg {
	int state;
	int method;
	char *headers[12];
	int last_header;
	char *transaction_id;
	char *delimiter;
	int code_number;
	char *code_description;
	switch_size_t byte_start;
	switch_size_t byte_end;
	switch_size_t bytes;
	switch_size_t payload_bytes;
	int range_star; /* range-end is '*' */
	char *last_p;
	char *payload;
	struct msrp_msg *next;
} msrp_msg_t;

typedef struct {
	char *remote_path;
	char *remote_accept_types;
	char *remote_accept_wrapped_types;
	char *remote_setup;
	char *remote_file_selector;
	char *local_path;
	char *local_accept_types;
	char *local_accept_wrapped_types;
	char *local_setup;
	char *local_file_selector;
	int local_port;
	char *call_id;
	msrp_msg_t *msrp_msg;
	msrp_msg_t *last_msg;
	switch_mutex_t *mutex;
	switch_size_t msrp_msg_buffer_size;
	switch_size_t msrp_msg_count;
	switch_socket_t *socket;
} msrp_session_t;

switch_status_t msrp_init();
switch_status_t msrp_destroy();
msrp_session_t *msrp_session_new(switch_memory_pool_t *pool);
switch_status_t msrp_session_destroy(msrp_session_t *session);
switch_status_t msrp_session_push_msg(msrp_session_t *session, msrp_msg_t *msg);
msrp_msg_t *msrp_session_pop_msg(msrp_session_t *session);
switch_status_t msrp_send(switch_socket_t *sock, msrp_msg_t *msg);

void load_msrp_apis_and_applications(switch_loadable_module_interface_t **moudle_interface);

#endif
