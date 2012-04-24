/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2011, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Seven Du <dujinfang@gmail.com>
 *
 * mod_rtsp -- RTSP Streaming
 *
 */
#include <switch.h>


SWITCH_MODULE_LOAD_FUNCTION(mod_rtsp_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_rtsp_shutdown);
SWITCH_MODULE_DEFINITION(mod_rtsp, mod_rtsp_load, mod_rtsp_shutdown, NULL);

#define rtsp_set_flag_locked(obj, flag) assert(obj->flag_mutex != NULL);\
switch_mutex_lock(obj->flag_mutex);\
(obj)->flags[flag] = 1;\
switch_mutex_unlock(obj->flag_mutex);
#define rtsp_set_flag(obj, flag) (obj)->flags[flag] = 1
#define rtsp_clear_flag(obj, flag) (obj)->flags[flag] = 0
#define rtsp_clear_flag_locked(obj, flag) switch_mutex_lock(obj->flag_mutex); (obj)->flags[flag] = 0; switch_mutex_unlock(obj->flag_mutex);
#define rtsp_test_flag(obj, flag) ((obj)->flags[flag] ? 1 : 0)

typedef enum {
	TFLAG_IO,
	TFLAG_CHANGE_MEDIA,
	TFLAG_OUTBOUND,
	TFLAG_READING,
	TFLAG_WRITING,
	TFLAG_HUP,
	TFLAG_RTP,
	TFLAG_BYE,
	TFLAG_ANS,
	TFLAG_EARLY_MEDIA,
	TFLAG_READY,
	TFLAG_NAT,
	TFLAG_SDP,
	TFLAG_VIDEO,
	TFLAG_PROXY_MEDIA,
	TFLAG_AUTOFLUSH_DURING_BRIDGE,
	/* No new flags below this line */
	TFLAG_MAX
} TFLAGS;

switch_endpoint_interface_t *rtsp_endpoint_interface;

struct private_object {
	switch_mutex_t *rtsp_mutex;
	switch_mutex_t *flag_mutex;
	uint8_t flags[TFLAG_MAX];
	switch_payload_t audio_recv_pt;
	switch_payload_t video_recv_pt;
	switch_core_session_t *session;
	switch_channel_t *channel;
	switch_frame_t read_frame;
	switch_codec_t read_codec;
	switch_codec_t write_codec;
	switch_rtp_t *rtp_session;
	uint32_t ssrc;
	uint32_t video_ssrc;
	// rtsp_profile_t *profile;
	char *local_sdp_audio_ip;
	switch_port_t local_sdp_audio_port;
	char *remote_sdp_audio_ip;
	switch_port_t remote_sdp_audio_port;
	char *adv_sdp_audio_ip;
	switch_port_t adv_sdp_audio_port;
	char *proxy_sdp_audio_ip;
	switch_port_t proxy_sdp_audio_port;
	char *rtpip;

	/** VIDEO **/
	switch_frame_t video_read_frame;
	switch_codec_t video_read_codec;
	switch_codec_t video_write_codec;
	switch_rtp_t *video_rtp_session;
	switch_port_t adv_sdp_video_port;
	switch_port_t local_sdp_video_port;
	switch_payload_t video_pt;
	uint32_t video_codec_ms;
	char *remote_sdp_video_ip;
	switch_port_t remote_sdp_video_port;
	char *video_fmtp_out;
	uint32_t video_count;
	int q850_cause;
	char *remote_ip;
	int remote_port;
	switch_rtp_bug_flag_t rtp_bugs;
	switch_codec_implementation_t read_impl;
	switch_codec_implementation_t write_impl;
	switch_caller_profile_t *caller_profile;
	const char *context;
	const char *destination_number;
};

typedef struct private_object private_object_t;

/* parse state machine */
enum {
	RTSP_ST_WAIT_HEADER,
	RTSP_ST_PARSE_HEADER,
	RTSP_ST_WAIT_BODY,
	RTSP_ST_DONE,
	RTSP_ST_ERROR,
};

enum {
	RTSP_M_UNKNOWN,
	RTSP_M_OPTIONS,
	RTSP_M_DESCRIBE,
	RTSP_M_SETUP,
	RTSP_M_PLAY,
	RTSP_M_STOP,
	RTSP_M_TEARDOWN,
	RTSP_M_REPLY
};

enum {
	RTSP_H_USER_AGENT,
	RTSP_H_CSEQ,
	RTSP_H_TRANSPORT,
	RTSP_H_SESSION,
	RTSP_H_UNKNOWN
};

typedef struct rtsp_msg {
	int parse_state;
	int method;
	char *headers[12];
	int last_header;
	char *full_path;
	char *path;
	int cseq;
	uint code;
	char *code_description;
	char *last_p;
	int content_length;
	char *body;
} rtsp_msg_t;


static switch_status_t rtsp_on_init(switch_core_session_t *session);

static switch_status_t rtsp_on_exchange_media(switch_core_session_t *session);
static switch_status_t rtsp_on_soft_execute(switch_core_session_t *session);
// static switch_call_cause_t rtsp_outgoing_channel(switch_core_session_t *session, switch_event_t *var_event,
// 	switch_caller_profile_t *outbound_profile, switch_core_session_t **new_session,
// 	switch_memory_pool_t **pool, switch_originate_flag_t flags, switch_call_cause_t *cancel_cause);
static switch_status_t rtsp_read_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id);
static switch_status_t rtsp_write_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id);
static switch_status_t rtsp_read_video_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id);
static switch_status_t rtsp_write_video_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id);
static switch_status_t rtsp_kill_channel(switch_core_session_t *session, int sig);

/* BODY OF THE MODULE */
/*************************************************************************************************************************************************************/

/*
   State methods they get called when the state changes to the specific state
   returning SWITCH_STATUS_SUCCESS tells the core to execute the standard state method next
   so if you fully implement the state you can return SWITCH_STATUS_FALSE to skip it.
*/

static switch_status_t rtsp_on_init(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_assert(tech_pvt != NULL);

	tech_pvt->read_frame.buflen = SWITCH_RTP_MAX_BUF_LEN;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s RTSP INIT\n", switch_channel_get_name(channel));

	/* Move channel's state machine to ROUTING */

	switch_channel_set_state(channel, CS_ROUTING);
	assert(switch_channel_get_state(channel) != CS_INIT);

	return status;
}

static switch_status_t rtsp_on_routing(switch_core_session_t *session)
{
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	// switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_assert(tech_pvt != NULL);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s RTSP ROUTING\n",
					  switch_channel_get_name(switch_core_session_get_channel(session)));

	return SWITCH_STATUS_SUCCESS;
}


static switch_status_t rtsp_on_reset(switch_core_session_t *session)
{
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_assert(tech_pvt != NULL);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s RTSP RESET\n",
					  switch_channel_get_name(switch_core_session_get_channel(session)));

	return SWITCH_STATUS_SUCCESS;
}


static switch_status_t rtsp_on_hibernate(switch_core_session_t *session)
{
	private_object_t *tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt != NULL);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s RTSP HIBERNATE\n",
					  switch_channel_get_name(switch_core_session_get_channel(session)));

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t rtsp_on_execute(switch_core_session_t *session)
{
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_assert(tech_pvt != NULL);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s RTSP EXECUTE\n",
					  switch_channel_get_name(switch_core_session_get_channel(session)));

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t rtsp_on_destroy(switch_core_session_t *session)
{
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_channel_t *channel = switch_core_session_get_channel(session);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s RTSP DESTROY\n", switch_channel_get_name(channel));

	if (tech_pvt) {

		if (switch_core_codec_ready(&tech_pvt->read_codec)) {
			switch_core_codec_destroy(&tech_pvt->read_codec);
		}

		if (switch_core_codec_ready(&tech_pvt->write_codec)) {
			switch_core_codec_destroy(&tech_pvt->write_codec);
		}

		if (switch_core_codec_ready(&tech_pvt->video_read_codec)) {
			switch_core_codec_destroy(&tech_pvt->video_read_codec);
		}

		if (switch_core_codec_ready(&tech_pvt->video_write_codec)) {
			switch_core_codec_destroy(&tech_pvt->video_write_codec);
		}

		switch_core_session_unset_read_codec(session);
		switch_core_session_unset_write_codec(session);

		if (tech_pvt->video_rtp_session) {
			switch_rtp_destroy(&tech_pvt->video_rtp_session);
		}

		if (tech_pvt->rtp_session) {
			switch_rtp_destroy(&tech_pvt->rtp_session);
		}
	}

	return SWITCH_STATUS_SUCCESS;

}

switch_status_t rtsp_on_hangup(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Channel %s hanging up\n",
					  switch_channel_get_name(channel));

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t rtsp_on_exchange_media(switch_core_session_t *session)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "SOFIA EXCHANGE_MEDIA\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t rtsp_on_soft_execute(switch_core_session_t *session)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "SOFIA SOFT_EXECUTE\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t rtsp_read_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id)
{
	private_object_t *tech_pvt = switch_core_session_get_private(session);
	// switch_channel_t *channel = switch_core_session_get_channel(session);
	// switch_status_t status;

	switch_assert(tech_pvt != NULL);

	tech_pvt->read_frame.datalen = 0;
	switch_set_flag(&tech_pvt->read_frame, SFF_CNG);

	// rtsp_set_flag_locked(tech_pvt, TFLAG_READING);
	//
	// status = switch_rtp_zerocopy_read_frame(tech_pvt->rtp_session, &tech_pvt->read_frame, flags);
	//
	// rtsp_clear_flag_locked(tech_pvt, TFLAG_READING);
	// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "reading frame %d\n", stream_id);

	*frame = &tech_pvt->read_frame;
	return SWITCH_STATUS_BREAK;
}

static switch_status_t rtsp_write_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id)
{
	private_object_t *tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt != NULL);

	switch_rtp_write_frame(tech_pvt->rtp_session, frame);

	rtsp_clear_flag_locked(tech_pvt, TFLAG_WRITING);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "write frame %d\n", stream_id);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t rtsp_read_video_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id)
{
	private_object_t *tech_pvt = switch_core_session_get_private(session);
	// switch_status_t status;

	tech_pvt->read_frame.datalen = 0;
	switch_set_flag(&tech_pvt->read_frame, SFF_CNG);

	// rtsp_set_flag_locked(tech_pvt, TFLAG_READING);
	//
	// 	status = switch_rtp_zerocopy_read_frame(tech_pvt->rtp_session, &tech_pvt->read_frame, flags);
	//
	// 	rtsp_clear_flag_locked(tech_pvt, TFLAG_READING);
	// 	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "reading frame %d\n", stream_id);
	switch_yield(1000000);
	*frame = &tech_pvt->read_frame;
	return SWITCH_STATUS_BREAK;
}

static switch_status_t rtsp_write_video_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id)
{
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_channel_t *channel = switch_core_session_get_channel(session);
	int wrote = 0;

	switch_assert(tech_pvt != NULL);

	while (!(tech_pvt->video_write_codec.implementation && switch_rtp_ready(tech_pvt->video_rtp_session))) {
		if (switch_channel_ready(channel)) {
			switch_yield(10000);
		} else {
			return SWITCH_STATUS_GENERR;
		}
	}

	if (rtsp_test_flag(tech_pvt, TFLAG_HUP)) {
		return SWITCH_STATUS_FALSE;
	}

	if (!rtsp_test_flag(tech_pvt, TFLAG_RTP)) {
		return SWITCH_STATUS_GENERR;
	}

	if (!rtsp_test_flag(tech_pvt, TFLAG_IO)) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (!switch_test_flag(frame, SFF_CNG)) {
		wrote = switch_rtp_write_frame(tech_pvt->video_rtp_session, frame);
	}

	return wrote > 0 ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_GENERR;
}

static switch_status_t rtsp_kill_channel(switch_core_session_t *session, int sig)
{
	private_object_t *tech_pvt = switch_core_session_get_private(session);

	if (!tech_pvt) {
		return SWITCH_STATUS_FALSE;
	}

	switch (sig) {
	case SWITCH_SIG_BREAK:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "SIG BREAK\n");

		if (switch_rtp_ready(tech_pvt->rtp_session)) {
			switch_rtp_break(tech_pvt->rtp_session);
		}
		if (switch_rtp_ready(tech_pvt->video_rtp_session)) {
			switch_rtp_break(tech_pvt->video_rtp_session);
		}
		break;
	case SWITCH_SIG_KILL:
	default:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "SIG KILL\n");

		rtsp_clear_flag_locked(tech_pvt, TFLAG_IO);
		rtsp_set_flag_locked(tech_pvt, TFLAG_HUP);

		if (switch_rtp_ready(tech_pvt->rtp_session)) {
			switch_rtp_kill_socket(tech_pvt->rtp_session);
		}
		if (switch_rtp_ready(tech_pvt->video_rtp_session)) {
			switch_rtp_kill_socket(tech_pvt->video_rtp_session);
		}
		break;
	}
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t rtsp_tech_codec(switch_core_session_t *session) {
	switch_channel_t *channel = switch_core_session_get_channel(session);
	private_object_t *tech_pvt = switch_core_session_get_private(session);
	switch_codec_implementation_t read_impl = { 0 };
	switch_codec_implementation_t write_impl = { 0 };

	switch_core_session_get_write_impl(session, &write_impl);
	switch_core_session_get_read_impl(session, &read_impl);

	if (switch_core_codec_init(&tech_pvt->write_codec,
			"PCMU",
			NULL,
			write_impl.samples_per_second,
			write_impl.microseconds_per_packet / 1000,
			1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			NULL, switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Audio Codec Activation Success\n");
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Audio Codec Activation Fail\n");
		switch_channel_set_variable(channel, SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE, "Audio codec activation failed");
		return SWITCH_STATUS_FALSE;
	}

	switch_core_session_set_write_codec(session, &tech_pvt->write_codec);

	if (switch_core_codec_init(&tech_pvt->read_codec,
			"PCMU",
			NULL,
			read_impl.samples_per_second,
			read_impl.microseconds_per_packet / 1000,
			1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			NULL, switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Audio Codec Activation Success\n");
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Audio Codec Activation Fail\n");
		switch_channel_set_variable(channel, SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE, "Audio codec activation failed");
		return SWITCH_STATUS_FALSE;
	}

	switch_core_session_set_read_codec(session, &tech_pvt->read_codec);

	if (switch_core_codec_init(&tech_pvt->video_write_codec,
			"H264", //tech_pvt->video_rm_encoding,
			NULL,//"packetization-mode=1;profile-level-id=42E01F;sprop-parameter-sets=J0LgH5ZUCg/QgAATiAAEk+BC,KM4GDMg=", //tech_pvt->video_rm_fmtp,
			90000, //tech_pvt->video_rm_rate,
			0,
			1,
			SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			NULL, switch_core_session_get_pool(tech_pvt->session)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(tech_pvt->session), SWITCH_LOG_ERROR, "Can't load codec?\n");
		return SWITCH_STATUS_FALSE;
	} else {

		tech_pvt->video_read_frame.rate = 90000;
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(tech_pvt->session), SWITCH_LOG_DEBUG, "Set VIDEO Codec %s %s/%d %d ms\n",
						  switch_channel_get_name(tech_pvt->channel), "H264", 90000, 0);
		// tech_pvt->video_read_frame.codec = &tech_pvt->video_read_codec;

		tech_pvt->video_fmtp_out = switch_core_session_strdup(tech_pvt->session, tech_pvt->video_write_codec.fmtp_out);

		tech_pvt->video_write_codec.agreed_pt = 96;
		tech_pvt->video_read_codec.agreed_pt = 96;
		// switch_core_session_set_video_read_codec(tech_pvt->session, &tech_pvt->video_read_codec);
		switch_core_session_set_video_write_codec(session, &tech_pvt->video_write_codec);

		switch_channel_set_flag(channel, CF_HOLD);
		switch_channel_set_flag(channel, CF_VIDEO);

		switch_channel_set_variable(channel, "rtsp_use_video_codec_name", "H264");
		switch_channel_set_variable(channel, "rtsp_use_video_codec_fmtp", "packetization-mode=1;profile-level-id=42E01F;sprop-parameter-sets=J0LgH5ZUCg/QgAATiAAEk+BC,KM4GDMg=");
		switch_channel_set_variable_printf(channel, "rtsp_use_video_codec_rate", "%d", 90000);
		switch_channel_set_variable_printf(channel, "rtsp_use_video_codec_ptime", "%d", 0);
	}

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t rtsp_new_channel(switch_core_session_t **new_session, int remote_video_rtp_port, const char *destination_number) {
	switch_core_session_t *session;
	switch_channel_t *channel = NULL;
	private_object_t *tech_pvt = NULL;
	switch_status_t status;
	const char *err;
	const char *ip;

	if (rtsp_endpoint_interface) {
		session = switch_core_session_request(rtsp_endpoint_interface, SWITCH_CALL_DIRECTION_INBOUND, SOF_NONE, NULL);
	}

	if (!session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Maximum Calls In Progress\n");
		goto fail;
	}

	if (!(tech_pvt = (private_object_t *) switch_core_session_alloc(session, sizeof(private_object_t)))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Hey where is my memory pool?\n");
		switch_core_session_destroy(&session);
		goto fail;
	}

	switch_mutex_init(&tech_pvt->rtsp_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
	switch_mutex_init(&tech_pvt->flag_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));

	channel = tech_pvt->channel = switch_core_session_get_channel(session);

	if ((tech_pvt->caller_profile =
		 switch_caller_profile_new(switch_core_session_get_pool(session), "rtsp",
								   "XML", "caller-id",
								   "rtsp-cid-number", NULL, NULL, NULL, NULL, "mod_rtsp", "default", destination_number)) != 0) {
		char name[128];
		switch_snprintf(name, sizeof(name), "rtsp/%s", destination_number);
		switch_channel_set_name(channel, name);
		switch_channel_set_caller_profile(channel, tech_pvt->caller_profile);

		switch_core_session_add_stream(session, NULL);

		tech_pvt->session = session;
		tech_pvt->channel = channel;
		// switch_channel_set_cap(tech_pvt->channel, CC_MEDIA_ACK);
		// switch_channel_set_cap(tech_pvt->channel, CC_BYPASS_MEDIA);
		// switch_channel_set_cap(tech_pvt->channel, CC_PROXY_MEDIA);
		// switch_channel_set_cap(tech_pvt->channel, CC_JITTERBUFFER);
		// switch_channel_set_cap(tech_pvt->channel, CC_FS_RTP);
		// switch_channel_set_cap(tech_pvt->channel, CC_QUEUEABLE_DTMF_DELAY);

		switch_core_session_set_private(session, tech_pvt);
	}

	if ((status = rtsp_tech_codec(session)) != SWITCH_STATUS_SUCCESS) {
		switch_core_session_destroy(&session);
		return status;
	}

	ip = switch_core_get_variable("local_ip_v4");
	if (zstr(ip)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "no local_ip_v4?\n");
		ip = "127.0.0.1";
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "send to ip:port %s:%d=============================\n", ip, remote_video_rtp_port);

	tech_pvt->rtp_session = switch_rtp_new(ip, //local
		6668, //local
		ip, //remote
		4440, //remote
		0, // pt
		1,
		8000,
		SWITCH_RTP_FLAG_AUTOADJ | SWITCH_RTP_FLAG_DATAWAIT | SWITCH_RTP_FLAG_VIDEO,
		NULL, &err, switch_core_session_get_pool(session));

	if (!tech_pvt->rtp_session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "llllllllllllllllll---------------%s-----!\n", err);
		return SWITCH_STATUS_FALSE;
	}

	tech_pvt->video_rtp_session = switch_rtp_new(ip, //local
		6666, //local
		ip, //remote
		remote_video_rtp_port, //remote
		96, // pt
		1,
		90000,
		SWITCH_RTP_FLAG_AUTOADJ | SWITCH_RTP_FLAG_DATAWAIT | SWITCH_RTP_FLAG_VIDEO,
		NULL, &err, switch_core_session_get_pool(session));

	if (!tech_pvt->video_rtp_session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "llllllllllllllllll---------------%s-----!\n", err);
		return SWITCH_STATUS_FALSE;
	}

	rtsp_set_flag(tech_pvt, TFLAG_IO);
	rtsp_set_flag(tech_pvt, TFLAG_RTP);

	switch_rtp_activate_rtcp(tech_pvt->rtp_session, 1000, 4441);
	switch_rtp_activate_rtcp(tech_pvt->video_rtp_session, 1000, remote_video_rtp_port + 1);

	switch_channel_set_variable_printf(channel, "rtsp_remote_video_rtp_port", "%d", remote_video_rtp_port);

	// let's go
	switch_channel_set_state(channel, CS_INIT);
	if (switch_core_session_thread_launch(session) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error spawning thread\n");
		switch_core_session_destroy(&session);
		return SWITCH_STATUS_FALSE;
	}

	*new_session = session;

fail:
	return SWITCH_STATUS_FALSE;
}

switch_status_t rtsp_answer_channel(switch_core_session_t *session) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Answer channel\n");
	return SWITCH_STATUS_SUCCESS;
}

#define RTSP_BUFF_SIZE 1023 /* 1024 - 1 */

static struct {
	int running;
	int debug;
	// switch_mutex_t *mutex;
	char *ip;
	switch_port_t port;
	switch_socket_t *sock;
	switch_thread_t *thread;
} globals;

typedef struct worker_helper{
	int debug;
	switch_memory_pool_t *pool;
	switch_socket_t *sock;
} worker_helper_t;

SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_ip, globals.ip);

static void *SWITCH_THREAD_FUNC rtsp_listener(switch_thread_t *thread, void *obj);

static void close_socket(switch_socket_t ** sock)
{
	// switch_mutex_lock(globals.sock_mutex);
	if (*sock) {
		switch_socket_shutdown(*sock, SWITCH_SHUTDOWN_READWRITE);
		switch_socket_close(*sock);
		*sock = NULL;
	}
	// switch_mutex_unlock(globals.sock_mutex);
}

switch_status_t rtsp_init()
{
	switch_memory_pool_t *pool;
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;
	switch_status_t rv;
	switch_sockaddr_t *sa;
	switch_socket_t *sock;

	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "OH OH no pool\n");
		return SWITCH_STATUS_FALSE;
	}

	memset(&globals, 0, sizeof(globals));
	set_global_ip("0.0.0.0");
	globals.port = (switch_port_t)8554;

	rv = switch_sockaddr_info_get(&sa, globals.ip, SWITCH_INET, globals.port, 0, pool);
	if (rv)
		goto sock_fail;
	rv = switch_socket_create(&sock, switch_sockaddr_get_family(sa), SOCK_STREAM, SWITCH_PROTO_TCP, pool);
	if (rv)
		goto sock_fail;
	rv = switch_socket_opt_set(sock, SWITCH_SO_REUSEADDR, 1);
	if (rv)
		goto sock_fail;
	rv = switch_socket_bind(sock, sa);
	if (rv)
		goto sock_fail;
	rv = switch_socket_listen(sock, 5);
	if (rv)
		goto sock_fail;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Socket up listening on %s:%u\n", globals.ip, globals.port);

	globals.running = 1;
	globals.debug = 1;
	globals.sock = sock;

	switch_threadattr_create(&thd_attr, pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_thread_create(&thread, thd_attr, rtsp_listener, sock, pool);

	globals.thread = thread;
	return SWITCH_STATUS_SUCCESS;

sock_fail:
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Socket Error! Could not listen on %s:%u\n", globals.ip, globals.port);
	return SWITCH_STATUS_FALSE;
}

switch_status_t rtsp_destroy()
{
	switch_status_t st;
	switch_socket_t *sock;
	globals.running = 0;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "destroy thread\n");

	sock = globals.sock;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "socket error\n");

	close_socket(&sock);
	if (globals.thread) switch_thread_join(&st, globals.thread);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "destroy thread done\n");

	globals.thread = NULL;
	return st;
}

rtsp_msg_t *rtsp_parse(char *buf, switch_memory_pool_t *pool)
{
	char *p;
	int argc;
	char *argv[6];
	char root_path[] = "/";
	rtsp_msg_t *rtsp_msg = NULL;

	if (!rtsp_msg) {
		switch_zmalloc(rtsp_msg, sizeof(rtsp_msg_t));
		switch_assert(rtsp_msg);
		rtsp_msg->parse_state = RTSP_ST_WAIT_HEADER;
	}

	if ((p = strstr(buf, "\r\n")) == NULL) {
		rtsp_msg->parse_state = RTSP_ST_ERROR;
		return rtsp_msg;
	}

	*p = '\0';

	// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s\n", buf);

	argc = switch_separate_string(buf, ' ', argv, (sizeof(argv) / sizeof(argv[0])));

	if (argc != 3) {
		rtsp_msg->parse_state = RTSP_ST_ERROR;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid rtsp header!\n");
		return rtsp_msg;
	}

	if (strncmp(argv[2], "RTSP/1.0", 8) && strncmp(argv[0], "RTSP/1.0", 8)) {
		rtsp_msg->parse_state = RTSP_ST_ERROR;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unsupported RTSP Version!\n");
		return rtsp_msg;
	}

	buf = p + 2;
	p = argv[0];

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s\n", p);

	if (!strncmp(p, "OPTIONS", 7)) {
		rtsp_msg->method = RTSP_M_OPTIONS;
	} else if (!strncmp(p, "DESCRIBE", 8)) {
		rtsp_msg->method = RTSP_M_DESCRIBE;
	} else if (!strncmp(p, "SETUP", 5)) {
		rtsp_msg->method = RTSP_M_SETUP;
	} else if (!strncmp(p, "PLAY", 4)) {
		rtsp_msg->method = RTSP_M_PLAY;
	} else if (!strncmp(p, "STOP", 4)) {
		rtsp_msg->method = RTSP_M_PLAY;
	} else if (!strncmp(p, "TEARDOWN", 8)) {
		rtsp_msg->method = RTSP_M_TEARDOWN;
	} else if (!strncmp(p, "RTSP/1.0", 8)) {
		rtsp_msg->method = RTSP_M_REPLY;
		rtsp_msg->code = atoi(argv[1]);
		rtsp_msg->code_description = switch_core_strdup(pool, argv[2]);
		p = argv[2];
		goto parse_headers;
	} else {
		rtsp_msg->parse_state = RTSP_ST_ERROR;
		return rtsp_msg;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s\n", p);

	p = argv[1];

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s\n", p);

	if (rtsp_msg->method != RTSP_M_REPLY && strncasecmp(p, "rtsp://", 7)) {
		rtsp_msg->parse_state = RTSP_ST_ERROR;
		return rtsp_msg;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s\n", p);

	rtsp_msg->full_path = switch_core_strdup(pool, argv[1]);

	p += 7; /* skipe rtsp:// */

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s\n", p);

	while(*p && (*p != '/')) p++;

	if (!*p) {
		rtsp_msg->path = switch_core_strdup(pool, root_path);
	} else {
		rtsp_msg->path = switch_core_strdup(pool, p);
	}


parse_headers:

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s\n", p);

	while(*buf) {
		p = strstr(buf, "\r\n");
		if (p) *p = '\0';

		if (!strncmp(buf, "CSeq:", 5)) {
			buf += 5;
			if (*buf == ' ') buf++;
			assert(buf);
			rtsp_msg->cseq = atoi(buf);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "p: %lx, %s\n", (long unsigned int)p, p);

			buf = p + 2;
		} else if (!strncmp(buf, "Transport:", 10)) {
			buf += 10;
			if (*buf == ' ') buf++;
			assert(buf);
			rtsp_msg->headers[RTSP_H_TRANSPORT] = switch_core_strdup(pool, buf);
			buf = p + 2;
		} else if (!strncmp(buf, "User-Agent:", 11)) {
			buf += 11;
			if (*buf == ' ') buf++;
			assert(buf);
			rtsp_msg->headers[RTSP_H_USER_AGENT] = switch_core_strdup(pool, buf);
			buf = p + 2;
		} else if (!strncmp(buf, "Session:", 8)) {
			buf += 8;
			if (*buf == ' ') buf++;
			assert(buf);
			rtsp_msg->headers[RTSP_H_SESSION] = switch_core_strdup(pool, buf);
			buf = p + 2;
		} else if (!strncmp(buf, "Content-Length:", 15)) {
			buf += 15;
			if (*buf == ' ') buf++;
			assert(buf);
			rtsp_msg->content_length = atoi(buf);
			if (rtsp_msg->content_length > 2048) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "hack me?\n");
				break;
				rtsp_msg->parse_state = RTSP_ST_ERROR;
			}
			rtsp_msg->parse_state = RTSP_ST_WAIT_BODY;
			return rtsp_msg;
		} else {
			buf = p + 2;
		}

		if (!p) {
			buf = NULL;
			break;
		}
	}

	rtsp_msg->parse_state = RTSP_ST_DONE;

	/* ignore anything else */
	return rtsp_msg;
}

void dump_buffer(char *buf, switch_size_t len, int line)
{
	int i, j, k = 0;
	char buff[RTSP_BUFF_SIZE * 2];
	// return;
	for(i=0,j=0; i<len; i++) {
		if (buf[i] == '\0') {
			buff[j++] = '\\';
			buff[j++] = '0';
		} else if(buf[i] == '\r') {
			buff[j++] = '\\';
			buff[j++] = 'r';
		} else if(buf[i] == '\n') {
			buff[j++] = '\\';
			buff[j++] = 'n';
			buff[j++] = '\n';
			k = 0;
		}
		 else {
			buff[j++] = buf[i];
		}
		if ((++k) %80 == 0) buff[j++] = '\n';
		if (j >= RTSP_BUFF_SIZE * 2) break;
	}
	buff[j] = '\0';
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%d:%" SWITCH_SIZE_T_FMT "DUMP:%s:DUMP\n", line, len, buff);

}

static void *SWITCH_THREAD_FUNC rtsp_worker(switch_thread_t *thread, void *obj)
{
	worker_helper_t *helper = (worker_helper_t *) obj;
	switch_socket_t *sock;
	switch_memory_pool_t *pool;
	char uuid[37] = {0};
	char buf[RTSP_BUFF_SIZE + 1] = { 0 };
	char *p;
	switch_size_t len = RTSP_BUFF_SIZE;
	rtsp_msg_t *rtsp_msg = NULL;
	char *msg = NULL;
	switch_size_t msg_len;
	char *ip = switch_core_get_variable("local_ip_v4");

	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;

	pool = helper->pool;
	sock = helper->sock;
	switch_socket_opt_set(sock, SWITCH_SO_TCP_NODELAY, TRUE);

	len = RTSP_BUFF_SIZE;
	p = buf;

	while(switch_socket_recv(sock, p, &len) == SWITCH_STATUS_SUCCESS) {
		int bytes_left = 0;
		char *end;

		dump_buffer(buf, p+len-buf, __LINE__);

		p[len] = '\0';
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "read bytes:%" SWITCH_SIZE_T_FMT "\n", len);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "%s\n", buf);

		if (*buf == '$') {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "binary protocol unsupported!\n");
			goto end;
		}

		if ((end = strstr(buf, "\r\n\r\n")) == NULL) {
			p += len;
			len = buf + RTSP_BUFF_SIZE - p;
			if (len <= 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "buffer overflow!\n");
				break;
			}
			continue;
		}

		*end = '\0';
		end +=4;

		rtsp_msg = rtsp_parse(buf, pool);

		switch_assert(rtsp_msg);
		if (rtsp_msg->parse_state == RTSP_ST_ERROR) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "rtsp parse error!\n");
			goto end;
		}

		if (helper->debug) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "state:%d, len:%" SWITCH_SIZE_T_FMT "\n", rtsp_msg->parse_state, len);
		}

		if (rtsp_msg->parse_state == RTSP_ST_DONE) {
			char *sdp;
			switch_size_t sdp_len;
			switch_size_t msg_len;
			const char *destination_number = rtsp_msg->path;
			char *p;
			int remote_video_port = 0;

			if (*destination_number == '/') destination_number++;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "-----%d %s\n", rtsp_msg->method, rtsp_msg->full_path);
			switch (rtsp_msg->method) {

				case RTSP_M_OPTIONS:
					msg = switch_mprintf("RTSP/1.0 200 OK\r\n"
						"CSeq: %d\r\n"
						"Allow: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE\r\n\r\n",
						rtsp_msg->cseq);
					assert(msg);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);

					msg_len = strlen(msg),
					switch_socket_send(sock, msg, &msg_len);
					switch_safe_free(msg);
					break;
				case RTSP_M_DESCRIBE:

					sdp = switch_mprintf("v=0\r\n"
						"o=- FS 1 IN IP4 %s\r\n"
						"s=Session streamed by FS\r\n"
						"i=test.264\r\n"
						"t=0 0\r\n"
						"a=tool:FS Streaming Media\r\n"
						"a=range:npt=0\r\n"
						"m=video 0 RTP/AVP 96\r\n"
						// "a=rtpmap:34 H263/90000\r\n"
						// "a=fmtp:34 QCIF=1;CIF=1;VGA=1\r\n"
						// "a=rtpmap:115 H263-1998/90000\r\n"
						// "a=fmtp:115 QCIF=1;CIF=1;VGA=1;I=1;J=1;T=1\r\n"
						// "c=IN IP4 192.168.0.75\r\n"
						"a=rtpmap:96 H264/90000\r\n"
						"a=fmtp:96 packetization-mode=1;profile-level-id=42E01F;sprop-parameter-sets=J0LgH5ZUCg/QgAATiAAEk+BC,KM4GDMg=\r\n"
						"a=control:trackID=0\r\n"
						// "m=audio 4440 RTP/AVP 0\r\n"
						// "a=rtpmap:0 PCMU\r\n"
						// "a=control:trackID=1\r\n"
						"\r\n", ip);
					assert(sdp);
					sdp_len = strlen(sdp);

					msg = switch_mprintf("RTSP/1.0 200 OK\r\n"
						"CSeq: %d\r\n"
						"Content-Base: %s\r\n"
						"Content-Type: application/sdp\r\n"
						"Content-Length: %d\r\n\r\n"
						"%s",

						rtsp_msg->cseq,
						rtsp_msg->full_path,
						sdp_len, sdp);
					assert(msg);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);

					msg_len = strlen(msg);
					switch_socket_send(sock, msg, &msg_len);

					switch_safe_free(sdp);
					switch_safe_free(msg);
					break;
				case RTSP_M_SETUP:
					if (!session) {

						if (rtsp_msg->headers[RTSP_H_TRANSPORT] &&
							(p = (strstr(rtsp_msg->headers[RTSP_H_TRANSPORT], "client_port=")))) {
							remote_video_port = atoi(p+12);
						}

						if (remote_video_port < 1) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "remote video rtp port not found!\n");
							goto end;
						}

						rtsp_new_channel(&session, remote_video_port, destination_number);

						if (!session) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "no session!\n");
							goto end;
						}

						channel = switch_core_session_get_channel(session);
						switch_set_string(uuid, switch_core_session_get_uuid(session));
					}

					msg = switch_mprintf("RTSP/1.0 200 OK\r\n"
						"CSeq: %d\r\n"
						"Transport: RTP/AVP;unicast;server_port=6666-6667;client_port=%d-%d\r\n"
						"Session: %s\r\n\r\n",
						rtsp_msg->cseq, remote_video_port, remote_video_port+1, uuid);
						assert(msg);
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);

					msg_len = strlen(msg),
					switch_socket_send(sock, msg, &msg_len);
					switch_safe_free(msg);
					break;
				case RTSP_M_PLAY:
					msg = switch_mprintf("RTSP/1.0 200 OK\r\n"
						"CSeq: %d\r\n"
						"Range: npt=0.000-\r\n"
						"Session: %s\r\n"
						"RTP-Info:%s\r\n\r\n",
						rtsp_msg->cseq, uuid, rtsp_msg->full_path);
					assert(msg);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);

					msg_len = strlen(msg),
					switch_socket_send(sock, msg, &msg_len);
					switch_safe_free(msg);
					break;
				case RTSP_M_TEARDOWN:
					msg = switch_mprintf("RTSP/1.0 200 OK\r\n"
						"CSeq: %d\r\n\r\n", rtsp_msg->cseq);
					assert(msg);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);

					msg_len = strlen(msg),
					switch_socket_send(sock, msg, &msg_len);
					switch_safe_free(msg);

					if (channel && switch_channel_ready(channel)) {
						switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
					}
					switch_yield(1000000);
					break;
				default:
					msg = switch_mprintf("RTSP/1.0 405 Method Not Allowed\r\n"
						"CSeq: %d\r\n"
						"Allow: DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE\r\n\r\n",
						rtsp_msg->cseq);
					assert(msg);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);

					msg_len = strlen(msg);
					switch_socket_send(sock, msg, &msg_len);
					switch_safe_free(msg);
			}
		}

		bytes_left = buf + len - end;

		if (bytes_left) {
			memmove(buf, end, bytes_left);
			len = RTSP_BUFF_SIZE - bytes_left;
			p = buf + len;
		} else { /* all buffer parsed */
			p = buf;
			len = RTSP_BUFF_SIZE;
		}
	}

end:

	if (!zstr(uuid)) {
		msg = switch_mprintf("TEARDOWN rtsp://192.168.0.173:8554/track1 RTSP/1.0\r\n"
			"CSeq: %d\r\n"
			"Session: %s\r\n",
			rtsp_msg->cseq, uuid);
		assert(msg);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);
		msg_len = strlen(msg);
		switch_socket_send(sock, msg, &msg_len);
		switch_safe_free(msg);
	}

	if (channel && switch_channel_ready(channel)) {
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
	}

	switch_yield(1000000);

	switch_socket_close(sock);
	switch_core_destroy_memory_pool(&pool);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "rtsp_worker down\n");

	return NULL;
}

static void *SWITCH_THREAD_FUNC rtsp_listener(switch_thread_t *thread, void *obj)
{
	switch_socket_t *listen_sock = (switch_socket_t *) obj;
	switch_status_t rv;
	switch_socket_t *sock;
	switch_memory_pool_t *pool = NULL;
	switch_threadattr_t *thd_attr = NULL;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "listener\n");

	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "OH OH no pool\n");
		return NULL;
	}

	switch_socket_opt_set(listen_sock, SWITCH_SO_TCP_NODELAY, TRUE);

	while (globals.running && (rv = switch_socket_accept(&sock, listen_sock, pool)) == SWITCH_STATUS_SUCCESS) {
		switch_memory_pool_t *worker_pool;
		worker_helper_t *helper;

		if (globals.debug > 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Connection Open\n");
		}

		if (switch_core_new_memory_pool(&worker_pool) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "OH OH no pool\n");
			return NULL;
		}
		switch_zmalloc(helper, sizeof(worker_helper_t));

		switch_assert(helper != NULL);
		helper->pool = worker_pool;
		helper->debug = globals.debug;
		helper->sock = sock;

		switch_threadattr_create(&thd_attr, pool);
		switch_threadattr_detach_set(thd_attr, 1);
		switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
		switch_thread_create(&thread, thd_attr, rtsp_worker, helper, worker_pool);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "new thread spawned!\n");
	}

	if (pool) switch_core_destroy_memory_pool(&pool);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "rtsp_listener down\n");

	return NULL;
}

static switch_status_t rtsp_receive_message(switch_core_session_t *session, switch_core_session_message_t *msg)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	private_object_t *tech_pvt = switch_core_session_get_private(session);
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (msg->message_id == SWITCH_MESSAGE_INDICATE_SIGNAL_DATA) {
		goto end;
	}

	if (switch_channel_down(channel) || !tech_pvt) {
		status = SWITCH_STATUS_FALSE;
		goto end;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg_id: %d\n", msg->message_id);

	/* ones that do not need to lock rtsp mutex */
	switch (msg->message_id) {
	case SWITCH_MESSAGE_INDICATE_APPLICATION_EXEC:
		break;
	case SWITCH_MESSAGE_INDICATE_PROXY_MEDIA:
		{
			if (switch_rtp_ready(tech_pvt->rtp_session)) {
				if (msg->numeric_arg) {
					switch_rtp_set_flag(tech_pvt->rtp_session, SWITCH_RTP_FLAG_PROXY_MEDIA);
				} else {
					switch_rtp_clear_flag(tech_pvt->rtp_session, SWITCH_RTP_FLAG_PROXY_MEDIA);
				}
			}
		}
		break;

	case SWITCH_MESSAGE_INDICATE_JITTER_BUFFER:
		break;
	case SWITCH_MESSAGE_INDICATE_DEBUG_AUDIO:
		{
			if (switch_rtp_ready(tech_pvt->rtp_session) && !zstr(msg->string_array_arg[0]) && !zstr(msg->string_array_arg[1])) {
				int32_t flags = 0;
				if (!strcasecmp(msg->string_array_arg[0], "read")) {
					flags |= SWITCH_RTP_FLAG_DEBUG_RTP_READ;
				} else if (!strcasecmp(msg->string_array_arg[0], "write")) {
					flags |= SWITCH_RTP_FLAG_DEBUG_RTP_WRITE;
				} else if (!strcasecmp(msg->string_array_arg[0], "both")) {
					flags |= SWITCH_RTP_FLAG_DEBUG_RTP_READ | SWITCH_RTP_FLAG_DEBUG_RTP_WRITE;
				}

				if (flags) {
					if (switch_true(msg->string_array_arg[1])) {
						switch_rtp_set_flag(tech_pvt->rtp_session, flags);
					} else {
						switch_rtp_clear_flag(tech_pvt->rtp_session, flags);
					}
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid Options\n");
				}
			}
		}
		status = SWITCH_STATUS_FALSE;
		goto end;
	case SWITCH_MESSAGE_INDICATE_TRANSCODING_NECESSARY:
		goto end;
	case SWITCH_MESSAGE_INDICATE_BRIDGE:
		goto end;
	case SWITCH_MESSAGE_INDICATE_UNBRIDGE:
		goto end;
	case SWITCH_MESSAGE_INDICATE_AUDIO_SYNC:
		if (switch_rtp_ready(tech_pvt->rtp_session)) {
			rtp_flush_read_buffer(tech_pvt->rtp_session, SWITCH_RTP_FLUSH_ONCE);
		}
		goto end;
	case SWITCH_MESSAGE_INDICATE_CLEAR_PROGRESS:
		goto end;
	case SWITCH_MESSAGE_INDICATE_RINGING:
		switch_channel_mark_ring_ready(channel);
	case SWITCH_MESSAGE_INDICATE_ANSWER:
		switch_channel_mark_answered(channel);
		status = rtsp_answer_channel(session);
		break;
	case SWITCH_MESSAGE_INDICATE_PROGRESS:
		switch_channel_mark_pre_answered(channel);
		break;
	default:
		break;
	}

	/* ones that do need to lock rtsp mutex */
	switch_mutex_lock(tech_pvt->rtsp_mutex);

	if (switch_channel_down(channel) || !tech_pvt) {
		status = SWITCH_STATUS_FALSE;
		goto end_lock;
	}

	switch (msg->message_id) {
	case SWITCH_MESSAGE_INDICATE_VIDEO_REFRESH_REQ:
		break;
	case SWITCH_MESSAGE_INDICATE_BROADCAST:
		break;
	case SWITCH_MESSAGE_INDICATE_NOMEDIA:
		break;
	case SWITCH_MESSAGE_INDICATE_AUDIO_DATA:
		break;
	case SWITCH_MESSAGE_INDICATE_MEDIA_REDIRECT:
		break;
	case SWITCH_MESSAGE_INDICATE_UDPTL_MODE:
	case SWITCH_MESSAGE_INDICATE_T38_DESCRIPTION:
		break;
	case SWITCH_MESSAGE_INDICATE_REQUEST_IMAGE_MEDIA:
	case SWITCH_MESSAGE_INDICATE_MEDIA:
		break;
	case SWITCH_MESSAGE_INDICATE_PHONE_EVENT:
	case SWITCH_MESSAGE_INDICATE_SIMPLIFY:
		break;
	case SWITCH_MESSAGE_INDICATE_DISPLAY:
		break;
	case SWITCH_MESSAGE_INDICATE_HOLD:
		break;
	case SWITCH_MESSAGE_INDICATE_UNHOLD:
		break;
	default:
		break;
	}

  end_lock:

	switch_mutex_unlock(tech_pvt->rtsp_mutex);

  end:

	if (switch_channel_down(channel) || !tech_pvt) {
		status = SWITCH_STATUS_FALSE;
	}

	return status;
}

static switch_status_t rtsp_receive_event(switch_core_session_t *session, switch_event_t *event)
{
	struct private_object *tech_pvt = switch_core_session_get_private(session);
	char *body;

	switch_assert(tech_pvt != NULL);

	if (!(body = switch_event_get_body(event))) {
		body = "";
	}

	return SWITCH_STATUS_SUCCESS;
}

struct channel_loop_helper {
	switch_socket_t *sock;
	switch_core_session_t *session;
	switch_rtp_t *rtp_session;
	switch_rtp_t **video_rtp_session;
	int running;
};

static void *SWITCH_THREAD_FUNC channel_loop(switch_thread_t *thread, void *obj)
{
	struct channel_loop_helper *helper = obj;
	switch_socket_t *sock = helper->sock;
	switch_core_session_t *session = helper->session;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_rtp_t *video_rtp_session = *(helper->video_rtp_session);
	switch_frame_t frame;
	switch_status_t status;

	while(switch_channel_ready(channel) && helper->running) {
		if (!video_rtp_session) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "yield\n");

			switch_yield(1000000);
			video_rtp_session = *(helper->video_rtp_session);
			continue;
		}

		status = switch_rtp_zerocopy_read_frame(video_rtp_session, &frame, SWITCH_IO_FLAG_NONE);

		if (switch_channel_test_flag(channel, CF_BREAK)) {
			switch_channel_clear_flag(channel, CF_BREAK);
			break;
		}

		switch_ivr_parse_all_events(session);

		{
			uint8_t nri;
			uint8_t fragment_type;
			uint8_t nal_type;
			uint8_t start_bit;
			uint8_t *hdr;
			
			hdr           = frame.data;
			nri           = hdr[0] & 0x60;
			fragment_type = hdr[0] & 0x1f;
			nal_type      = hdr[1] & 0x1f;
			start_bit     = hdr[1] & 0x80;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"%x %x | ft: %d, nt: %d, sb: %d len: %d m: %d ts: %ld\n",
				hdr[0], hdr[1], fragment_type, nal_type, start_bit, frame.datalen, frame.m, frame.timestamp);
		}

		if (!SWITCH_READ_ACCEPTABLE(status)) {
			break;
		}

		if (switch_channel_test_flag(channel, CF_VIDEO)) {
			switch_core_session_write_video_frame(session, &frame, SWITCH_IO_FLAG_NONE, 0);
		}
	}

	helper->running = 0;

	close_socket(&sock);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "closing channel\n");

	return NULL;
}

SWITCH_STANDARD_APP(play_rtsp_function)
{
	char *host, *port_name, *path, *user = NULL, *pass = NULL, *msg;
	switch_socket_t *new_sock;
	switch_sockaddr_t *sa;
	switch_port_t port = 554;
	int argc = 0, cseq = 1;
	char *argv[80] = { 0 };
	char *mydata;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_memory_pool_t *pool = switch_core_session_get_pool(session);
	switch_rtp_t *video_rtp_session = NULL;
	const char *err;
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr;
	struct channel_loop_helper helper;

	char buf[RTSP_BUFF_SIZE + 1] = { 0 };
	char *p;
	switch_size_t len = RTSP_BUFF_SIZE;
	rtsp_msg_t *rtsp_msg = NULL;
	switch_size_t msg_len;

	channel = switch_core_session_get_channel(session);

	if (data && (mydata = switch_core_session_strdup(session, data))) {
		argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (argc < 1) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Parse Error!\n");
		return;
	}

	path = argv[0];

	if (zstr(path) || strncasecmp(path, "rtsp://", 7)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid Path!\n");
		return;
	}

	host = switch_core_session_strdup(session, path + 7);

	if ((port_name = strrchr(host, ':'))) {
		*port_name++ = '\0';
		port = (switch_port_t) atoi(port_name);
	} else {
		port_name = strchr(host, '/');
		*port_name = '\0';
		port_name = NULL;
	}

	if (argc > 2) {
		user = argv[1];
		pass = argv[2];
	}

	switch_channel_set_variable(channel, "remote_rtsp_path", path);
	switch_channel_set_variable(channel, "remote_rtsp_host", host);
	switch_channel_set_variable_printf(channel, "remote_rtsp_port", "%d", port);

	if (switch_sockaddr_info_get(&sa, host, SWITCH_UNSPEC, port, 0, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Socket Error!\n");
		return;
	}

	if (switch_socket_create(&new_sock, switch_sockaddr_get_family(sa), SOCK_STREAM, SWITCH_PROTO_TCP, switch_core_session_get_pool(session))
		!= SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Socket Error!\n");
		return;
	}

	switch_socket_opt_set(new_sock, SWITCH_SO_KEEPALIVE, 1);
	switch_socket_opt_set(new_sock, SWITCH_SO_TCP_NODELAY, 1);

	if (switch_socket_connect(new_sock, sa) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Socket Error!\n");
		return;
	}

	helper.sock = new_sock;
	helper.session = session;
	helper.video_rtp_session = &video_rtp_session;

	switch_threadattr_create(&thd_attr, pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	helper.running = 1;
	switch_thread_create(&thread, thd_attr, channel_loop, &helper, pool);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s %s %s\n", path, user, pass);

	if (!switch_channel_ready(channel)) return;

	msg = switch_mprintf("OPTIONS %s RTSP/1.0\r\n"
		"CSeq: %d\r\n"
		"User-Agent: FreeSWITCH\r\n\r\n",
		path, cseq++);
	assert(msg);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);

	msg_len = strlen(msg),
	switch_socket_send(new_sock, msg, &msg_len);
	switch_safe_free(msg);

	p = buf;
	while(switch_socket_recv(new_sock, p, &len) == SWITCH_STATUS_SUCCESS) {
		int bytes_left = 0;
		char *end;

		dump_buffer(buf, p+len-buf, __LINE__);

		p[len] = '\0';
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "read bytes:%" SWITCH_SIZE_T_FMT "\n", len);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "%s\n", buf);

		if (*buf == '$') {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "binary protocol unsupported!\n");
			goto end;
		}

		if (rtsp_msg && rtsp_msg->parse_state == RTSP_ST_WAIT_BODY) {
			if (p+len-buf >= rtsp_msg->content_length) {
				*(buf + rtsp_msg->content_length - 2) = '\0';
				rtsp_msg->body = switch_core_strdup(pool, buf);
				p = buf + rtsp_msg->content_length + 1 - 2;
				if (*p == '\n') {
					p++;
					rtsp_msg->parse_state = RTSP_ST_DONE;
					end = p;
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "FixME!\n");
					break;
				}
			} else {
				p += len;
				len = buf + RTSP_BUFF_SIZE - p;
				if (len <= 0) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "buffer overflow!\n");
					break;
				}
				continue;
			}
		} else if ((end = strstr(buf, "\r\n\r\n")) == NULL) {
			p += len;
			len = buf + RTSP_BUFF_SIZE - p;
			if (len <= 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "buffer overflow!\n");
				break;
			}
			continue;
		} else {
			*end = '\0';
			end +=4;
		}

		rtsp_msg = rtsp_parse(buf, pool);

		switch_assert(rtsp_msg);
		if (rtsp_msg->parse_state == RTSP_ST_ERROR) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "rtsp parse error!\n");
			goto end;
		}

		if (1) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "state:%d, len:%" SWITCH_SIZE_T_FMT "\n", rtsp_msg->parse_state, len);
		}

		if (rtsp_msg->parse_state == RTSP_ST_WAIT_BODY) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "l: %d\n", rtsp_msg->content_length);

			if (p+len-end >= rtsp_msg->content_length) {
				p = end + rtsp_msg->content_length - 2;
				*p = '\0';
				rtsp_msg->body = switch_core_strdup(pool, end);
				if (*++p == '\n') {
					rtsp_msg->parse_state = RTSP_ST_DONE;
					end = ++p;
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "FixME!\n");
					break;
				}
			} else {
				p += len;
				len = buf + RTSP_BUFF_SIZE - p;
				if (len <= 0) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "buffer overflow!\n");
					break;
				}
				continue;
			}
		}

		if (rtsp_msg->parse_state == RTSP_ST_DONE) {
			switch_size_t msg_len;
			const char *destination_number = rtsp_msg->path;
			// char *p;
			// int remote_video_port = 0;

			if (*destination_number == '/') destination_number++;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "-----%d %s\n", rtsp_msg->method, rtsp_msg->full_path);
			switch (rtsp_msg->method) {
				case RTSP_M_TEARDOWN:
					msg = switch_mprintf("RTSP/1.0 200 OK\r\n"
						"CSeq: %d\r\n\r\n", rtsp_msg->cseq);
					assert(msg);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);

					msg_len = strlen(msg),
					switch_socket_send(new_sock, msg, &msg_len);
					switch_safe_free(msg);

					if (channel && switch_channel_ready(channel)) {
						switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
					}
					switch_yield(1000000);
					break;
				case RTSP_M_REPLY:
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "cseq: %d, code: %d\n", rtsp_msg->cseq, rtsp_msg->code);

					if (rtsp_msg->cseq == 1 && rtsp_msg->code == 200) {
						msg = switch_mprintf("DESCRIBE %s RTSP/1.0\r\n"
							"CSeq: %d\r\n"
							"User-Agent: FreeSWITCH\r\n"
							"Accept: application/sdp\r\n\r\n",
							path, cseq++);
						assert(msg);
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);

						msg_len = strlen(msg),
						switch_socket_send(new_sock, msg, &msg_len);
						switch_safe_free(msg);
					} else if (rtsp_msg->cseq == 2 && rtsp_msg->code == 401) {
						msg = switch_mprintf("DESCRIBE %s RTSP/1.0\r\n"
							"CSeq: %d\r\n"
							"User-Agent: FreeSWITCH\r\n"
							"Authorization: Basic YWRtaW46MTIzNDU=\r\n"
							"Accept: application/sdp\r\n\r\n",
							path, cseq++);
						assert(msg);
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);

						msg_len = strlen(msg),
						switch_socket_send(new_sock, msg, &msg_len);
						switch_safe_free(msg);
					} else if (rtsp_msg->cseq == 3 && rtsp_msg->code == 200) {

#define IP1 "192.168.0.103"
						video_rtp_session = video_rtp_session ? video_rtp_session :
							switch_rtp_new(IP1, //local
								9998, //local
								host, //remote
								9998, //remote
								96, // pt
								1,
								90000,
								SWITCH_RTP_FLAG_AUTOADJ | SWITCH_RTP_FLAG_DATAWAIT | SWITCH_RTP_FLAG_VIDEO,
								NULL, &err, pool);

						if (!video_rtp_session) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "llllllllllllllllll---------------%s-----!\n", err);
						}

						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "body:%s\n", rtsp_msg->body);
						msg = switch_mprintf("SETUP %s/TrackID=1 RTSP/1.0\r\n"
							"CSeq: %d\r\n"
							"User-Agent: FreeSWITCH\r\n"
							"Authorization: Basic YWRtaW46MTIzNDU=\r\n"
							"Transport: RTP/AVP;unicast;client_port=9998-9999\r\n\r\n",
							path, cseq++);
						assert(msg);
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);
						msg_len = strlen(msg),
						switch_socket_send(new_sock, msg, &msg_len);
						switch_safe_free(msg);
					} else if (rtsp_msg->cseq == 4 && rtsp_msg->code == 200) {
						msg = switch_mprintf("PLAY %s RTSP/1.0\r\n"
							"CSeq: %d\r\n"
							"User-Agent: FreeSWITCH\r\n"
							"Authorization: Basic YWRtaW46MTIzNDU=\r\n"
							"Session: %s\r\n"
							"Range: npt=0.000-\r\n\r\n",
							path, cseq++, rtsp_msg->headers[RTSP_H_SESSION]);
						assert(msg);
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);
						msg_len = strlen(msg),
						switch_socket_send(new_sock, msg, &msg_len);
						switch_safe_free(msg);
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "body:%s\n", rtsp_msg->body);
					}
					break;
				default:
					msg = switch_mprintf("RTSP/1.0 405 Method Not Allowed\r\n"
						"CSeq: %d\r\n"
						"Allow: DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE\r\n\r\n",
						rtsp_msg->cseq);
					assert(msg);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "msg: %s\n", msg);

					msg_len = strlen(msg);
					switch_socket_send(new_sock, msg, &msg_len);
					switch_safe_free(msg);
			}
		}

		bytes_left = buf + len - end;

		if (bytes_left) {
			memmove(buf, end, bytes_left);
			len = RTSP_BUFF_SIZE - bytes_left;
			p = buf + len;
		} else { /* all buffer parsed */
			p = buf;
			len = RTSP_BUFF_SIZE;
		}
	}

end:
	close_socket(&new_sock);
	while (helper.running) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "waiting thread to stop");
	}
	if (video_rtp_session) switch_rtp_destroy(&video_rtp_session);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ending play_rtsp\n");
}

switch_io_routines_t rtsp_io_routines = {
	/*.outgoing_channel */ NULL,
	/*.read_frame */ rtsp_read_frame,
	/*.write_frame */ rtsp_write_frame,
	/*.kill_channel */ rtsp_kill_channel,
	/*.send_dtmf */ NULL,
	/*.receive_message */ rtsp_receive_message,
	/*.receive_event */ rtsp_receive_event,
	/*.state_change */ NULL,
	/*.read_video_frame */ rtsp_read_video_frame,
	/*.write_video_frame */ rtsp_write_video_frame
};

switch_state_handler_table_t rtsp_event_handlers = {
	/*.on_init */ rtsp_on_init,
	/*.on_routing */ rtsp_on_routing,
	/*.on_execute */ rtsp_on_execute,
	/*.on_hangup */ rtsp_on_hangup,
	/*.on_exchange_media */ rtsp_on_exchange_media,
	/*.on_soft_execute */ rtsp_on_soft_execute,
	/*.on_consume_media */ NULL,
	/*.on_hibernate */ rtsp_on_hibernate,
	/*.on_reset */ rtsp_on_reset,
	/*.on_park */ NULL,
	/*.on_reporting */ NULL,
	/*.on_destroy */ rtsp_on_destroy
};


SWITCH_MODULE_LOAD_FUNCTION(mod_rtsp_load)
{
	switch_application_interface_t *app_interface;

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	rtsp_endpoint_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_ENDPOINT_INTERFACE);
	rtsp_endpoint_interface->interface_name = "rtsp";
	rtsp_endpoint_interface->io_routines = &rtsp_io_routines;
	rtsp_endpoint_interface->state_handler = &rtsp_event_handlers;

	rtsp_init();

	SWITCH_ADD_APP(app_interface, "play_rtsp", "play rtsp audio/videos", "play rtsp audio/videos", play_rtsp_function, "user:pass@<ip>[:<port>][/<path>]", SAF_SUPPORT_NOMEDIA);

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_rtsp_shutdown)
{
	rtsp_destroy();
	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */
