/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <string.h>
#include <zephyr.h>
#include <stdlib.h>
#include <net/socket.h>
#include <net/net_ip.h>
#include <modem/bsdlib.h>
#include <net/tls_credentials.h>
#include <net/http_client.h>
#include <modem/at_cmd.h>
#include <tinycrypt/hmac_prng.h>
#include <tinycrypt/hmac.h>
#include <tinycrypt/constants.h>
#include <sys/base64.h>
#include <logging/log.h>
#include <net/aws_jobs.h> /* for enum execution_status */
#include "fota_client_mgmt.h"

LOG_MODULE_REGISTER(fota_client_mgmt, CONFIG_MODEM_FOTA_LOG_LEVEL);

/* TODO: The nrf-<IMEI> format is for testing/certification only
 * Device ID will become a GUID for production code.
 */
#define DEV_ID_PREFIX "nrf-"
#define IMEI_LEN (15)
#define DEV_ID_BUFF_SIZE (sizeof(DEV_ID_PREFIX) + IMEI_LEN + 2)

/* NOTE: The AT command "request revision identification"
 * can return up to 2048 bytes.  That is limited here to a more
 * realistic value.
 */
#define MFW_VER_BUFF_SIZE (128)

/* NOTE: The header is static from the device point of view
 * so there is no need to build JSON and encode it every time.
 */
/* JWT header: {"alg":"HS256","typ":"JWT"} */
#define JWT_HEADER_B64 "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
/* JWT payload: {"deviceIdentifier":"nrf-<IMEI>"} */
#define JWT_PAYLOAD_TEMPLATE "{\"deviceIdentifier\":\"%s\"}"
#define JWT_PAYLOAD_BUFF_SIZE (sizeof(JWT_PAYLOAD_TEMPLATE) + DEV_ID_BUFF_SIZE)

/* TODO: For initial certification, a hard-coded shared secret will be used.
 * For a production release, the plan is to use the modem's built in key
 * to do the signing.
 */
#define SHARED_SECRET { \
		0x32, 0x39, 0x34, 0x41, 0x34, 0x30, 0x34, 0x45, \
		0x36, 0x33, 0x35, 0x32, 0x36, 0x36, 0x35, 0x35, \
		0x36, 0x41, 0x35, 0x38, 0x36, 0x45, 0x33, 0x32, \
		0x37, 0x32, 0x33, 0x34, 0x37, 0x35, 0x33, 0x37 }

#define GET_BASE64_LEN(n) (((4 * n / 3) + 3) & ~3)
/* <b64 header>.<b64 payload>.<b64 signature><NULL> */
#define JWT_BUFF_SIZE (sizeof(JWT_HEADER_B64) + \
		       GET_BASE64_LEN(sizeof(jwt_payload)) + 1 + \
		       GET_BASE64_LEN(sizeof(jwt_sig)) + 1)

static int generate_jwt(const char * const device_id, char ** jwt_out);
static int generate_auth_header(const char * const jwt, char ** auth_hdr_out);
static int generate_host_header(const char * const hostname,
				char ** host_hdr_out);
static void base64_url_format(char * const base64_string);
static char * get_base64url_string(const char * const input,
				   const size_t input_size);
static char * get_device_id_string(void);
static char * get_mfw_version_string(void);
static int get_signature(const uint8_t * const data_in,
			 const size_t data_in_size,
			 uint8_t * data_out,
			 size_t const data_out_size);
static void http_response_cb(struct http_response *rsp,
			     enum http_final_call final_data,
			     void *user_data);
static int tls_setup(int fd, const char * const tls_hostname);
static int do_connect(int * const fd, const char * const hostname,
		      const uint16_t port_num, bool use_fota_apn);
static int parse_pending_job_response(const char * const resp_buff,
				      struct fota_client_mgmt_job * const job);

#define API_HOSTNAME "static.api.beta.nrfcloud.com"
#define API_PORT 443
#define API_HTTP_TIMEOUT_MS (30000)

#define HTTP_PROTOCOL "HTTP/1.1"

// TODO: determine if it is worth adding JSON parsing library to
#define JOB_ID_BEGIN_STR	"\"jobId\":\""
#define JOB_ID_END_STR		"\""
#define FW_URI_BEGIN_STR	"\"uris\":[\""
#define FW_URI_END_STR		"\"]"
#define FW_PATH_PREFIX		"v1/firmwares/modem/"
#define FW_HOSTNAME 		API_HOSTNAME

#define AUTH_HDR_BEARER_TEMPLATE "Authorization: Bearer %s\r\n"
#define HOST_HDR_TEMPLATE	 "Host: %s\r\n"

#define API_DEV_STATE_URL_TEMPLATE	"/v1/devices/%s/state"
#define API_DEV_STATE_CONTENT_TYPE	"application/json"
#define API_DEV_STATE_HDR_ACCEPT	"accept: */*\r\n"
#define API_DEV_STATE_BODY_TEMPLATE	"{\"reported\":{\"device\":{" \
					"\"deviceInfo\":{\"modemFirmware\":\"%s\"}," \
					"\"serviceInfo\":{\"fota_v1\":[\"MODEM\"]}}}}"

#define API_UPDATE_JOB_URL_PREFIX	"/v1/fota-job-execution-statuses/"
#define API_UPDATE_JOB_CONTENT_TYPE	"application/json"
#define API_UPDATE_JOB_HDR_ACCEPT	"accept: */*\r\n"
#define API_UPDATE_JOB_BODY_TEMPLATE	"{\"status\":\"%s\"}"

#define API_GET_JOB_URL_TEMPLATE	"/v1/fota-jobs/device/%s/latest-pending"
#define API_GET_JOB_CONTENT_TYPE 	"*/*"
#define API_GET_JOB_HDR_ACCEPT		"accept: application/json\r\n"

// NOTE:
//       The endpoint address isn't actually important... traffic is routed
//       based on the certs, so ensure that correct dev/beta/prod certs are
//       installed on the device.
// TODO: switch to PROD endpoint: "a2n7tk1kp18wix-ats.iot.us-east-1.amazonaws.com"
//       BETA endpoint to be used for environment stability
#define JITP_HOSTNAME "a1jtaajis3u27i-ats.iot.us-east-1.amazonaws.com"
#define JITP_HOSTNAME_TLS 	JITP_HOSTNAME
#define JITP_PORT		8443
#define JITP_URL 	    	"/topics/jitp?qos=1"
#define JITP_CONTENT_TYPE   	"*/*"
#define JITP_HDR_CONNECTION 	"Connection: close\r\n"
#define JITP_HDR_HOST		"Host: " JITP_HOSTNAME ":" \
				STRINGIFY(JITP_PORT) "\r\n"
#define JITP_HTTP_TIMEOUT_MS	(15000)

#define SOCKET_PROTOCOL IPPROTO_TLS_1_2

enum http_status {
	HTTP_STATUS_UNHANDLED = -1,
	HTTP_STATUS_NONE = 0,
	HTTP_STATUS_OK = 200,
	HTTP_STATUS_ACCEPTED = 202,
	HTTP_STATUS_BAD_REQ = 400,
	HTTP_STATUS_UNAUTH = 401,
	HTTP_STATUS_FORBIDDEN = 403,
	HTTP_STATUS_NOT_FOUND = 404,
	HTTP_STATUS_UNPROC_ENTITY = 422,
};

enum http_req_type {
	HTTP_REQ_TYPE_UNHANDLED,
	HTTP_REQ_TYPE_PROVISION,
	HTTP_REQ_TYPE_GET_JOB,
	HTTP_REQ_TYPE_UPDATE_JOB,
	HTTP_REQ_TYPE_DEV_STATE,
};

struct http_user_data {
	enum http_req_type type;
	union {
		struct fota_client_mgmt_job * job;
	} data;
};

// TODO: this is copied from aws_jobs.c, find better way to share this?
/** @brief Mapping of enum to strings for Job Execution Status. */
static const char *job_status_strings[] = {
	[AWS_JOBS_QUEUED]      = "QUEUED",
	[AWS_JOBS_IN_PROGRESS] = "IN_PROGRESS",
	[AWS_JOBS_SUCCEEDED]   = "SUCCEEDED",
	[AWS_JOBS_FAILED]      = "FAILED",
	[AWS_JOBS_TIMED_OUT]   = "TIMED_OUT",
	[AWS_JOBS_REJECTED]    = "REJECTED",
	[AWS_JOBS_REMOVED]     = "REMOVED",
	[AWS_JOBS_CANCELED]    = "CANCELED"
};

#define JOB_STATUS_STRING_COUNT (sizeof(job_status_strings) / \
				 sizeof(*job_status_strings))

#define HTTP_RX_BUF_SIZE (4096)
static char http_rx_buf[HTTP_RX_BUF_SIZE];
static enum http_status http_resp_status;

/* API hostname (if != NULL overrides the default) */
static char *api_hostname;
/* API port number (if != 0 overrides the default) */
static uint16_t api_port;
/* FW API hostname (if != NULL overrides the default) */
static char *fw_api_hostname;

extern const char *fota_apn;

static int socket_apn_set(int fd, const char *apn)
{
	int err;
	size_t len;
	struct ifreq ifr = {0};

	__ASSERT_NO_MSG(apn);

	len = strlen(apn);
	if (len >= sizeof(ifr.ifr_name)) {
		LOG_ERR("Access point name is too long");
		return -EINVAL;
	}

	memcpy(ifr.ifr_name, apn, len);
	err = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, len);
	if (err) {
		LOG_ERR("Failed to bind socket, error: %d", errno);
		return -EINVAL;
	}

	return 0;
}

static int socket_timeouts_set(int fd)
{
	int err;

	/* Set socket send timeout to 60s (affects also TCP connect) */
	struct timeval send_timeout = {
		.tv_sec = 60,
		.tv_usec = 0
	};
	err = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
			 &send_timeout, sizeof(send_timeout));
	if (err) {
		LOG_ERR("Failed to set socket send timeout, error: %d", errno);
		return err;
	}

	/* Set socket receive timeout to 30s */
	struct timeval recv_timeout = {
		.tv_sec = 30,
		.tv_usec = 0
	};
	err = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
			 &recv_timeout, sizeof(recv_timeout));
	if (err) {
		LOG_ERR("Failed to set socket recv timeout, error: %d", errno);
		return err;
	}

	return 0;
}

static int do_connect(int * const fd, const char * const hostname,
		      const uint16_t port_num, bool use_fota_apn)
{
	int ret;
	const char *apn = NULL;
	struct addrinfo *addr_info;

	if (use_fota_apn && fota_apn != NULL && strlen(fota_apn) > 0) {
		apn = fota_apn;
	}

	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
		.ai_next =  apn ?
			&(struct addrinfo) {
				.ai_family    = AF_LTE,
				.ai_socktype  = SOCK_MGMT,
				.ai_protocol  = NPROTO_PDN,
				.ai_canonname = (char *)apn
			} : NULL,
	};

	/* Make sure fd is always initialized when this function is called */
	*fd = -1;

	ret = getaddrinfo(hostname, NULL, &hints, &addr_info);
	if (ret) {
		LOG_ERR("getaddrinfo() failed, error: %d", errno);
		return -EFAULT;
	} else {
		char peer_addr[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &net_sin(addr_info->ai_addr)->sin_addr,
			  peer_addr, INET_ADDRSTRLEN);
		LOG_DBG("getaddrinfo() %s", log_strdup(peer_addr));
	}

	((struct sockaddr_in *)addr_info->ai_addr)->sin_port = htons(port_num);

	*fd = socket(AF_INET, SOCK_STREAM, SOCKET_PROTOCOL);
	if (*fd == -1) {
		LOG_ERR("Failed to open socket, error: %d", errno);
		ret = -ENOTCONN;
		goto error_clean_up;
	}

	if (apn != NULL) {
		LOG_DBG("Setting up APN: %s", log_strdup(apn));
		ret = socket_apn_set(*fd, apn);
		if (ret) {
			ret = -EINVAL;
			goto error_clean_up;
		}
	}

	ret = tls_setup(*fd, hostname);
	if (ret) {
		ret = -EACCES;
		goto error_clean_up;
	}

	ret = socket_timeouts_set(*fd);
	if (ret) {
		LOG_ERR("Failed to set socket timeouts, error: %d", errno);
		ret = -EINVAL;
		goto error_clean_up;
	}

	LOG_DBG("Connecting to %s", log_strdup(hostname));

	ret = connect(*fd, addr_info->ai_addr, sizeof(struct sockaddr_in));
	if (ret) {
		LOG_ERR("Failed to connect socket, error: %d", errno);
		ret = -ECONNREFUSED;
		goto error_clean_up;
	} else {
		freeaddrinfo(addr_info);
		return 0;
	}

error_clean_up:
	freeaddrinfo(addr_info);
	if (*fd > -1) {
		(void)close(*fd);
		*fd = -1;
	}
	return ret;
}

int fota_client_provision_device(void)
{
	int fd;
	int ret;
	struct http_request req;
	struct http_user_data prov_data = { .type = HTTP_REQ_TYPE_PROVISION };
	const char * headers[] = { JITP_HDR_CONNECTION, JITP_HDR_HOST, NULL };

	memset(&req, 0, sizeof(req));
	req.method = HTTP_POST;
	req.url = JITP_URL;
	req.host = JITP_HOSTNAME;
	req.protocol = HTTP_PROTOCOL;
	req.content_type_value = JITP_CONTENT_TYPE;
	req.header_fields = headers;
	req.response = http_response_cb;
	req.recv_buf = http_rx_buf;
	req.recv_buf_len = sizeof(http_rx_buf);
	http_resp_status = HTTP_STATUS_NONE;

	ret = do_connect(&fd, JITP_HOSTNAME, JITP_PORT, false);
	if (ret) {
		return ret;
	}

	ret = http_client_req(fd, &req, JITP_HTTP_TIMEOUT_MS, &prov_data);
	LOG_DBG("http_client_req() returned: %d", ret);
	if (ret < 0) {
		ret = -EIO;
	} else if (http_resp_status == HTTP_STATUS_NONE) {
		/* No response means the device was NOT already provisioned,
		 * so provisioning should be occurring. Wait 30s before
		 * attemping an API call.
		 */
		ret = 0;
	} else if (http_resp_status == HTTP_STATUS_FORBIDDEN) {
		/* HTTP 403 is returned when device is already provisioned */
		ret = 1;
	} else {
		/* TODO: determine if there are any other error responses */
		ret = -ENOMSG;
	}

	(void)close(fd);

	return ret;
}

void fota_client_job_free(struct fota_client_mgmt_job * const job)
{
	if (!job) {
		return;
	}

	if (job->host) {
		k_free(job->host);
		job->host = NULL;
	}
	if (job->path) {
		k_free(job->path);
		job->path = NULL;
	}
	if (job->id) {
		k_free(job->id);
		job->id = NULL;
	}
}

int fota_client_get_pending_job(struct fota_client_mgmt_job * const job)
{
	if (!job) {
		return -EINVAL;
	}

	int fd;
	int ret;
	struct http_user_data job_data = { .type = HTTP_REQ_TYPE_GET_JOB };
	struct http_request req;
	size_t buff_size;
	char * jwt = NULL;
	char * url = NULL;
	char * auth_hdr = NULL;
	char * host_hdr = NULL;
	char * device_id = get_device_id_string();
	char * hostname = API_HOSTNAME;
	uint16_t port = API_PORT;

	if (api_hostname != NULL) {
		hostname = api_hostname;
	}
	if (api_port != 0) {
		port = api_port;
	}

	memset(job,0,sizeof(*job));
	job_data.data.job = job;

	if (!device_id) {
		ret = -ENXIO;
		goto clean_up;
	}

	ret = generate_jwt(device_id,&jwt);
	if (ret < 0){
		LOG_ERR("Failed to generate JWT, error: %d", ret);
		goto clean_up;
	}

	/* Format API URL with device ID */
	buff_size = sizeof(API_GET_JOB_URL_TEMPLATE) + strlen(device_id);
	url = k_calloc(buff_size, 1);
	if (!url) {
		ret = -ENOMEM;
		goto clean_up;
	}
	ret = snprintk(url, buff_size, API_GET_JOB_URL_TEMPLATE, device_id);
	if (ret < 0 || ret >= buff_size) {
		LOG_ERR("Could not format URL");
		ret = -ENOBUFS;
		goto clean_up;
	}

	/* Format auth header with JWT */
	ret = generate_auth_header(jwt, &auth_hdr);
	if (ret) {
		LOG_ERR("Could not format HTTP auth header");
		goto clean_up;
	}

	ret = generate_host_header(hostname, &host_hdr);
	if (ret) {
		LOG_ERR("Could not generate Host header");
		goto clean_up;
	}

	LOG_DBG("URL: %s\n", log_strdup(url));

	const char * headers[] = { API_GET_JOB_HDR_ACCEPT,
				   auth_hdr,
				   host_hdr,
				   NULL};

	/* Init HTTP request */
	memset(http_rx_buf,0,HTTP_RX_BUF_SIZE);
	memset(&req, 0, sizeof(req));
	req.method = HTTP_GET;
	req.url = url;
	req.host = hostname;
	req.protocol = HTTP_PROTOCOL;
	req.content_type_value = API_GET_JOB_CONTENT_TYPE;
	req.header_fields = headers;
	req.response = http_response_cb;
	req.recv_buf = http_rx_buf;
	req.recv_buf_len = sizeof(http_rx_buf);
	http_resp_status = HTTP_STATUS_NONE;

	ret = do_connect(&fd, hostname, port, true);
	if (ret) {
		goto clean_up;
	}

	ret = http_client_req(fd, &req, JITP_HTTP_TIMEOUT_MS, &job_data);

	LOG_DBG("http_client_req() returned: %d", ret);

	if (ret < 0) {
		ret = -EIO;
	} else {
		ret = 0;
		if (http_resp_status == HTTP_STATUS_NOT_FOUND) {
			/* No pending job */
		} else if (http_resp_status == HTTP_STATUS_OK) {
			if (job->host && job->path && job->path) {
				job->status = AWS_JOBS_IN_PROGRESS;
			} else {
				/* Job info was returned but it was not
				 * able to be parsed */
				ret = -EBADMSG;
			}
		} else {
			LOG_ERR("HTTP status: %d", http_resp_status);
			ret = -ENODATA;
		}
	}

clean_up:
	if (fd > -1) {
		(void)close(fd);
	}
	if (jwt) {
		k_free(jwt);
	}
	if (device_id) {
		k_free(device_id);
	}
	if (url) {
		k_free(url);
	}
	if (auth_hdr) {
		k_free(auth_hdr);
	}
	if (host_hdr) {
		k_free(host_hdr);
	}

	return ret;
}

int fota_client_update_job(const struct fota_client_mgmt_job * job)
{
	if ( !job || !job->id ) {
		return -EINVAL;
	} else if (job->status >= JOB_STATUS_STRING_COUNT) {
		return -ENOENT;
	}

	int fd;
	int ret;
	struct http_user_data job_data = { .type = HTTP_REQ_TYPE_UPDATE_JOB };
	struct http_request req;
	size_t buff_size;
	char * jwt = NULL;
	char * url = NULL;
	char * auth_hdr =  NULL;
	char * host_hdr = NULL;
	char * payload = NULL;
	char * hostname = API_HOSTNAME;
	uint16_t port = API_PORT;

	if (api_hostname != NULL) {
		hostname = api_hostname;
	}
	if (api_port != 0) {
		port = api_port;
	}

	ret = generate_jwt(NULL,&jwt);
	if (ret < 0){
		LOG_ERR("Failed to generate JWT, error: %d", ret);
		goto clean_up;
	}

	/* Format API URL with job ID */
	buff_size = sizeof(API_UPDATE_JOB_URL_PREFIX) + strlen(job->id);
	url = k_calloc(buff_size, 1);
	if (!url) {
		ret = -ENOMEM;
		goto clean_up;
	}
	ret = snprintk(url, buff_size, "%s%s",
		       API_UPDATE_JOB_URL_PREFIX, job->id);
	if (ret < 0 || ret >= buff_size) {
		LOG_ERR("Could not format URL");
		ret = -ENOBUFS;
	}

	/* Format auth header with JWT */
	ret = generate_auth_header(jwt, &auth_hdr);
	if (ret) {
		LOG_ERR("Could not format HTTP auth header");
		goto clean_up;
	}

	ret = generate_host_header(hostname, &host_hdr);
	if (ret) {
		LOG_ERR("Could not generate Host header");
		goto clean_up;
	}

	/* Create payload */
	buff_size = sizeof(API_UPDATE_JOB_BODY_TEMPLATE) +
		    strlen(job_status_strings[job->status]);
	payload = k_calloc(buff_size,1);
	if (!payload) {
		ret = -ENOMEM;
		goto clean_up;
	}
	ret = snprintk(payload, buff_size, API_UPDATE_JOB_BODY_TEMPLATE,
		       job_status_strings[job->status]);
	if (ret < 0 || ret >= buff_size) {
		LOG_ERR("Could not format HTTP payload");
		ret = -ENOBUFS;
		goto clean_up;
	}

	LOG_DBG("URL: %s\n", log_strdup(url));
	LOG_DBG("Payload: %s\n", log_strdup(payload));

	const char * headers[] = { API_UPDATE_JOB_HDR_ACCEPT,
				   auth_hdr,
				   host_hdr,
				   NULL};

	/* Init HTTP request */
	memset(http_rx_buf,0,HTTP_RX_BUF_SIZE);
	memset(&req, 0, sizeof(req));
	req.method = HTTP_PATCH;
	req.url = url;
	req.host = hostname;
	req.protocol = HTTP_PROTOCOL;
	req.content_type_value = API_UPDATE_JOB_CONTENT_TYPE;
	req.header_fields = headers;
	req.payload = payload;
	req.payload_len = strlen(payload);
	req.response = http_response_cb;
	req.recv_buf = http_rx_buf;
	req.recv_buf_len = sizeof(http_rx_buf);
	http_resp_status = HTTP_STATUS_NONE;

	ret = do_connect(&fd, hostname, port, true);
	if (ret) {
		goto clean_up;
	}

	ret = http_client_req(fd, &req, API_HTTP_TIMEOUT_MS, &job_data);

	LOG_DBG("http_client_req() returned: %d", ret);

	if (ret < 0) {
		ret = -EIO;
	} else {
		ret = 0;
		if (http_resp_status != HTTP_STATUS_OK) {
			LOG_ERR("HTTP status: %d", http_resp_status);
			ret = -ENODATA;
		}
	}

clean_up:
	if (fd > -1) {
		(void)close(fd);
	}
	if (jwt) {
		k_free(jwt);
	}
	if (url) {
		k_free(url);
	}
	if (auth_hdr) {
		k_free(auth_hdr);
	}
	if (host_hdr) {
		k_free(host_hdr);
	}
	if (payload) {
		k_free(payload);
	}

	return ret;
}

int fota_client_set_device_state(void)
{
	int fd;
	int ret;
	struct http_request req;
	struct http_user_data dev_state = { .type = HTTP_REQ_TYPE_DEV_STATE };
	size_t buff_size;
	uint16_t port = API_PORT;
	char * jwt = NULL;
	char * url = NULL;
	char * auth_hdr =  NULL;
	char * host_hdr = NULL;
	char * payload = NULL;
	char * hostname = API_HOSTNAME;
	char * device_id = get_device_id_string();
	char * mfw_ver = get_mfw_version_string();

	if (api_hostname != NULL) {
		hostname = api_hostname;
	}
	if (api_port != 0) {
		port = api_port;
	}

	ret = generate_jwt(device_id,&jwt);
	if (ret < 0){
		LOG_ERR("Failed to generate JWT, error: %d", ret);
		goto clean_up;
	}

	/* Format API URL with device ID */
	buff_size = sizeof(API_DEV_STATE_URL_TEMPLATE) + strlen(device_id);
	url = k_calloc(buff_size, 1);
	if (!url) {
		ret = -ENOMEM;
		goto clean_up;
	}
	ret = snprintk(url, buff_size, API_DEV_STATE_URL_TEMPLATE, device_id);
	if (ret < 0 || ret >= buff_size) {
		LOG_ERR("Could not format URL");
		ret = -ENOBUFS;
		goto clean_up;
	}

	/* Format auth header with JWT */
	ret = generate_auth_header(jwt, &auth_hdr);
	if (ret) {
		LOG_ERR("Could not format HTTP auth header");
		goto clean_up;
	}

	ret = generate_host_header(hostname, &host_hdr);
	if (ret) {
		LOG_ERR("Could not generate Host header");
		goto clean_up;
	}

	/* Create payload */
	buff_size = sizeof(API_DEV_STATE_BODY_TEMPLATE) +
		    strlen(mfw_ver);
	payload = k_calloc(buff_size,1);
	if (!payload) {
		ret = -ENOMEM;
		goto clean_up;
	}
	ret = snprintk(payload, buff_size, API_DEV_STATE_BODY_TEMPLATE,
		       mfw_ver);
	if (ret < 0 || ret >= buff_size) {
		LOG_ERR("Could not format HTTP payload");
		ret = -ENOBUFS;
		goto clean_up;
	}

	LOG_DBG("URL: %s\n", log_strdup(url));
	LOG_DBG("Payload: %s\n", log_strdup(payload));

	const char * headers[] = { API_DEV_STATE_HDR_ACCEPT,
				   auth_hdr,
				   host_hdr,
				   NULL};

	/* Init HTTP request */
	memset(http_rx_buf,0,HTTP_RX_BUF_SIZE);
	memset(&req, 0, sizeof(req));
	req.method = HTTP_PATCH;
	req.url = url;
	req.host = hostname;
	req.protocol = HTTP_PROTOCOL;
	req.content_type_value = API_DEV_STATE_CONTENT_TYPE;
	req.header_fields = headers;
	req.payload = payload;
	req.payload_len = strlen(payload);
	req.response = http_response_cb;
	req.recv_buf = http_rx_buf;
	req.recv_buf_len = sizeof(http_rx_buf);
	http_resp_status = HTTP_STATUS_NONE;

	ret = do_connect(&fd, hostname, port, true);
	if (ret) {
		goto clean_up;
	}

	ret = http_client_req(fd, &req, API_HTTP_TIMEOUT_MS, &dev_state);

	LOG_DBG("http_client_req() returned: %d", ret);

	if (ret < 0) {
		ret = -EIO;
	} else {
		ret = 0;
		if (http_resp_status != HTTP_STATUS_ACCEPTED) {
			LOG_ERR("HTTP status: %d", http_resp_status);
			ret = -ENODATA;
		}
	}

clean_up:
	if (fd > -1) {
		(void)close(fd);
	}
	if (jwt) {
		k_free(jwt);
	}
	if (device_id) {
		k_free(device_id);
	}
	if (mfw_ver) {
		k_free(mfw_ver);
	}
	if (url) {
		k_free(url);
	}
	if (auth_hdr) {
		k_free(auth_hdr);
	}
	if (host_hdr) {
		k_free(host_hdr);
	}
	if (payload) {
		k_free(payload);
	}

	return ret;
}

static int generate_auth_header(const char * const jwt, char ** auth_hdr_out)
{
	if (!jwt || !auth_hdr_out)
	{
		return -EINVAL;
	}

	int ret;
	size_t buff_size = sizeof(AUTH_HDR_BEARER_TEMPLATE) + strlen(jwt);

	*auth_hdr_out = k_calloc(buff_size,1);
	if (!*auth_hdr_out) {
		return -ENOMEM;
	}
	ret = snprintk(*auth_hdr_out, buff_size, AUTH_HDR_BEARER_TEMPLATE, jwt);
	if (ret < 0 || ret >= buff_size) {
		k_free(*auth_hdr_out);
		*auth_hdr_out = NULL;
		return -ENOBUFS;
	}

	return 0;
}

static int generate_host_header(const char * const hostname,
				char ** host_hdr_out)
{
	int ret;
	size_t buff_size = sizeof(HOST_HDR_TEMPLATE) + strlen(hostname);

	if (!hostname || !host_hdr_out)	{
		return -EINVAL;
	}

	*host_hdr_out = k_calloc(buff_size, 1);
	if (!*host_hdr_out) {
		return -ENOMEM;
	}
	ret = snprintk(*host_hdr_out, buff_size, HOST_HDR_TEMPLATE, hostname);
	if (ret < 0 || ret >= buff_size) {
		k_free(*host_hdr_out);
		*host_hdr_out = NULL;
		return -ENOBUFS;
	}

	return 0;
}

static int generate_jwt(const char * const device_id, char ** jwt_out)
{
	if (!jwt_out)
	{
		return -EINVAL;
	}

	char jwt_payload[JWT_PAYLOAD_BUFF_SIZE];
	uint8_t jwt_sig[TC_SHA256_DIGEST_SIZE];
	int ret;
	char * jwt_buff;
	char * jwt_sig_b64;
	char * jwt_payload_b64;
	char * dev_id = NULL;
	size_t jwt_len = 0;

	*jwt_out = NULL;

	/* Get device ID if it was not provided */
	if (!device_id) {
		dev_id = get_device_id_string();
		if (!dev_id) {
			LOG_ERR("Could not get device ID string");
			return -ENODEV;
		}
	}

	/* Add device ID to JWT payload */
	ret = snprintk(jwt_payload, sizeof(jwt_payload),
		       JWT_PAYLOAD_TEMPLATE,
		       device_id ? device_id : dev_id);
	if (dev_id) {
		k_free(dev_id);
		dev_id = NULL;
	}
	if (ret < 0 || ret >= sizeof(jwt_payload)) {
		LOG_ERR("Could not format JWT payload");
		return -ENOBUFS;
	}

	LOG_DBG("JWT payload: %s\n", log_strdup(jwt_payload));

	/* Encode payload string to base64 */
	jwt_payload_b64 = get_base64url_string(jwt_payload,
					       strlen(jwt_payload));
	if (!jwt_payload_b64) {
		LOG_ERR("Could not encode JWT payload");
		return -ENOMSG;
	}

	/* Allocate output JWT buffer and add header and payload */
	jwt_buff = k_calloc(JWT_BUFF_SIZE,1);
	if (!jwt_buff){
		LOG_ERR("Could not allocate JWT buffer");
		k_free(jwt_payload_b64);
		return -ENOMEM;
	}

	ret = snprintk(jwt_buff,JWT_BUFF_SIZE,"%s.%s",
		       JWT_HEADER_B64, jwt_payload_b64);
	k_free(jwt_payload_b64);
	jwt_payload_b64 = NULL;
	if (ret < 0 || ret >= JWT_BUFF_SIZE) {
		LOG_ERR("Could not format JWT header and payload");
		k_free(jwt_buff);
		return -ENOBUFS;
	}
	jwt_len = ret;

	/* Get signature and append base64 encoded signature to JWT */
	ret = get_signature((uint8_t*)jwt_buff, strlen(jwt_buff),
			    jwt_sig, sizeof(jwt_sig));
	if (ret) {
		LOG_ERR("Error signing JWT, error: %d", ret);
		k_free(jwt_buff);
		return -EBADMSG;
	}

	jwt_sig_b64 = get_base64url_string(jwt_sig, TC_SHA256_DIGEST_SIZE);
	if (!jwt_sig_b64) {
		LOG_ERR("Could not encode JWT signature");
		k_free(jwt_buff);
		return -ENOMSG;
	}

	LOG_DBG("Signature: %s\n", log_strdup(jwt_sig_b64));

	ret = snprintk(&jwt_buff[jwt_len],
		       JWT_BUFF_SIZE,
		       ".%s",
		       jwt_sig_b64);
	k_free(jwt_sig_b64);
	jwt_sig_b64 = NULL;
	if (ret < 0 || ret >= (JWT_BUFF_SIZE-jwt_len)) {
		LOG_ERR("Could not format JWT signature");
		k_free(jwt_buff);
		return -ENOBUFS;
	}

	LOG_DBG("JWT: %s\n", log_strdup(jwt_buff));

	*jwt_out = jwt_buff;

	return strlen(*jwt_out);
}

static int get_signature(const uint8_t * const data_in,
			 const size_t data_in_size,
			 uint8_t * data_out,
			 size_t const data_out_size) {

	if (!data_in || !data_in_size || !data_out) {
		return -EINVAL;
	} else if (data_out_size < TC_SHA256_DIGEST_SIZE) {
		LOG_ERR("data_out must be >= %d bytes",
		       TC_SHA256_DIGEST_SIZE);
		return -ENOBUFS;
	}

	struct tc_hmac_state_struct hmac;
	char shared_secret[] = SHARED_SECRET;
	int ret;

	ret = tc_hmac_set_key(&hmac, shared_secret, sizeof(shared_secret));
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("tc_hmac_set_key failed, error: %d", ret);
		return -EACCES;
	}

	ret = tc_hmac_init(&hmac);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("tc_hmac_init failed, error: %d", ret);
	}

	ret = tc_hmac_update(&hmac, data_in, data_in_size);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("tc_hmac_update failed, error: %d", ret);
		return -EACCES;
	}

	ret = tc_hmac_final(data_out,data_out_size,&hmac);
	if (ret != TC_CRYPTO_SUCCESS) {
		LOG_ERR("tc_hmac_final failed, error: %d", ret);
		return -EACCES;
	}

	LOG_HEXDUMP_DBG(data_out, TC_SHA256_DIGEST_SIZE, "HMAC hex");

	return 0;
}

static char * get_device_id_string(void)
{
	int ret;
	enum at_cmd_state state;
	size_t dev_id_len;
	char * dev_id = k_calloc(DEV_ID_BUFF_SIZE,1);

	if (!dev_id) {
		LOG_ERR("Could not allocate memory for device ID");
		return NULL;
	}

	ret = snprintk(dev_id, DEV_ID_BUFF_SIZE,"%s", DEV_ID_PREFIX);
	if (ret < 0 || ret >= DEV_ID_BUFF_SIZE) {
		LOG_ERR("Could not format device ID");
		k_free(dev_id);
		return NULL;
	}
	dev_id_len = ret;

	at_cmd_init();

	ret = at_cmd_write("AT+CGSN",
			   &dev_id[dev_id_len],
			   DEV_ID_BUFF_SIZE - dev_id_len,
			   &state);
	if (ret) {
		LOG_ERR("Failed to get IMEI, error: %d", ret);
		k_free(dev_id);
		return NULL;
	}

	dev_id_len += IMEI_LEN; /* remove /r/n from AT cmd result */
	dev_id[dev_id_len] = 0;

	return dev_id;
}

static char * get_mfw_version_string(void)
{
	int ret;
	enum at_cmd_state state;
	char * mfw_version = k_calloc(MFW_VER_BUFF_SIZE,1);
	char * string_end;

	if (!mfw_version) {
		LOG_ERR("Could not allocate memory for modem FW version");
		return NULL;
	}

	at_cmd_init();

	ret = at_cmd_write("AT+CGMR",
			   mfw_version,
			   MFW_VER_BUFF_SIZE,
			   &state);
	if (ret) {
		LOG_ERR("Failed to get modem FW version, error: %d", ret);
		k_free(mfw_version);
		return NULL;
	}

	/* NULL terminate at /r */
	string_end = strchr(mfw_version, '\r');
	if (string_end) {
		*string_end = '\0';
	} else {
		mfw_version[MFW_VER_BUFF_SIZE-1] = '\0';
	}

	return mfw_version;
}

char * get_base64url_string(const char * const input, const size_t input_size)
{
	if (!input || !input_size) {
		LOG_ERR("Invalid input buffer");
		return NULL;
	}
	int ret;
	char * output_str;
	size_t output_str_len;

	(void)base64_encode(NULL,
			    0,
			    &output_str_len,
			    input,
			    input_size);
	if (output_str_len == ((size_t)-1)) {
		LOG_ERR("Unable to encode input string to base64");
		return NULL;
	}

	output_str = k_calloc(output_str_len+1,1);
	if (!output_str) {
		LOG_ERR("Unable to allocate memory for base64 string");
		return NULL;
	}
	ret = base64_encode(output_str,
			    output_str_len,
			    &output_str_len,
			    input,
			    input_size);
	if (ret) {
		LOG_ERR("Error encoding input string to base64, error: %d", ret);
		k_free(output_str);
		return NULL;
	}
	base64_url_format(output_str);

	return output_str;
}

void base64_url_format(char * const base64_string)
{
	if (base64_string == NULL) {
		return;
	}

	char * found = NULL;

	/* replace '+' with "-" */
	for(found = base64_string; (found = strchr(found,'+'));) {
		*found = '-';
	}

	/* replace '/' with "_" */
	for(found = base64_string; (found = strchr(found,'/'));) {
		*found = '_';
	}

	/* NULL terminate at first '=' pad character */
	found = strchr(base64_string, '=');
	if (found) {
		*found = '\0';
	}
}

static void http_response_cb(struct http_response *rsp,
			enum http_final_call final_data,
			void *user_data)
{
	int ret;
	struct http_user_data * usr = NULL;

	if (user_data) {
		usr = (struct http_user_data *)user_data;
	}

	if (final_data == HTTP_DATA_MORE) {
		LOG_DBG("HTTP: partial data received (%zd bytes)\n", rsp->data_len);
		if (rsp->body_start) {
			LOG_DBG("BODY %s\n", log_strdup(rsp->body_start));
		}
	} else if (final_data == HTTP_DATA_FINAL) {
		LOG_DBG("HTTP: All data received (%zd bytes)\n", rsp->data_len);
		if (rsp->data_len && rsp->body_found) {
			LOG_DBG("HTTP body:\n%s\n", log_strdup(rsp->body_start));
		}
		else if (rsp->body_found && !rsp->body_start) {
			LOG_DBG("HTTP rx:\n%s\n", log_strdup(rsp->recv_buf));
		}

		/* TODO: handle all statuses returned from API */
		if (strncmp(rsp->http_status,"Not Found", HTTP_STATUS_STR_SIZE) == 0) {
			http_resp_status = HTTP_STATUS_NOT_FOUND;
		} else if (strncmp(rsp->http_status,"Forbidden", HTTP_STATUS_STR_SIZE) == 0) {
			http_resp_status = HTTP_STATUS_FORBIDDEN;
		} else if (strncmp(rsp->http_status,"OK", HTTP_STATUS_STR_SIZE) == 0) {
			http_resp_status = HTTP_STATUS_OK;
		} else if (strncmp(rsp->http_status,"Unauthorized", HTTP_STATUS_STR_SIZE) == 0) {
			http_resp_status = HTTP_STATUS_UNAUTH;
		} else if (strncmp(rsp->http_status,"Bad Request", HTTP_STATUS_STR_SIZE) == 0) {
			http_resp_status = HTTP_STATUS_BAD_REQ;
		} else if (strncmp(rsp->http_status,"Unprocessable Entity", HTTP_STATUS_STR_SIZE) == 0) {
			http_resp_status = HTTP_STATUS_UNPROC_ENTITY;
		} else if (strncmp(rsp->http_status,"Accepted", HTTP_STATUS_STR_SIZE) == 0) {
			http_resp_status = HTTP_STATUS_ACCEPTED;
		} else {
			http_resp_status = HTTP_STATUS_UNHANDLED;
		}
		if (!usr) {
			LOG_ERR("HTTP response to unknown request: %s",
			       log_strdup(rsp->http_status));
			return;
		}

		LOG_DBG("HTTP response to request type %d: \"%s\"\n",
		       usr->type, log_strdup(rsp->http_status));

		switch (usr->type) {
		case HTTP_REQ_TYPE_GET_JOB:
			if (http_resp_status == HTTP_STATUS_OK) {
				ret = parse_pending_job_response(
					rsp->recv_buf,
					usr->data.job);
				if (ret) {
					LOG_ERR("Error parsing job information");
				}
			}
			break;
		case HTTP_REQ_TYPE_PROVISION:
		case HTTP_REQ_TYPE_UPDATE_JOB:
		case HTTP_REQ_TYPE_DEV_STATE:
		default:
			break;
		}
	}
}

int parse_pending_job_response(const char * const resp_buff,
			       struct fota_client_mgmt_job * const job)
{
	char * hostname;
	char * start;
	char * end;
	size_t len;
	int err;

	job->host = NULL;
	job->id = NULL;
	job->path = NULL;

	hostname = FW_HOSTNAME;
	if (fw_api_hostname != NULL) {
		hostname = fw_api_hostname;
	}

	job->host = k_calloc(strlen(hostname) + 1,1);
	if (!job->host) {
		err = -ENOMEM;
		goto error_clean_up;
	}
	strcpy(job->host,hostname);

	start = strstr(resp_buff,JOB_ID_BEGIN_STR);
	if (!start) {
		err =  -ENOMSG;
		goto error_clean_up;
	}

	start += strlen(JOB_ID_BEGIN_STR);
	end = strstr(start,JOB_ID_END_STR);
	if (!end) {
		err =  -ENOMSG;
		goto error_clean_up;
	}

	len = end - start;
	job->id = k_calloc(len + 1,1);
	strncpy(job->id,start,len);

	// Get URI/path
	start = strstr(resp_buff,FW_URI_BEGIN_STR);
	if (!start) {
		err =  -ENOMSG;
		goto error_clean_up;
	}

	start += strlen(FW_URI_BEGIN_STR);
	end = strstr(start,FW_URI_END_STR);
	if (!end) {
		err =  -ENOMSG;
		goto error_clean_up;
	}

	len = end - start;
	job->path = k_calloc(sizeof(FW_PATH_PREFIX) + len,1);
	if (!job->path) {
		err =  -ENOMEM;
		goto error_clean_up;
	}

	strncpy(job->path,
		FW_PATH_PREFIX,
		sizeof(FW_PATH_PREFIX));
	strncpy(job->path + strlen(FW_PATH_PREFIX),
		start,len);

	return 0;

error_clean_up:
	fota_client_job_free(job);
	return err;
}

int tls_setup(int fd, const char * const tls_hostname)
{
	int err;
	int verify;
	const sec_tag_t tls_sec_tag[] = {
		CONFIG_MODEM_FOTA_TLS_SECURITY_TAG,
	};

	enum {
		NONE = 0,
		OPTIONAL = 1,
		REQUIRED = 2,
	};

	verify = REQUIRED;

	err = setsockopt(fd, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
	if (err) {
		LOG_ERR("Failed to setup peer verification, error: %d", errno);
		return err;
	}

	err = setsockopt(fd, SOL_TLS, TLS_SEC_TAG_LIST, tls_sec_tag,
			 sizeof(tls_sec_tag));
	if (err) {
		LOG_ERR("Failed to setup TLS sec tag, error: %d", errno);
		return err;
	}

	if (tls_hostname) {
		err = setsockopt(fd, SOL_TLS, TLS_HOSTNAME, tls_hostname,
				 strlen(tls_hostname));
		if (err) {
			LOG_ERR("Failed to setup TLS hostname, error: %d", errno);
			return err;
		}
	}
	return 0;
}

char *get_api_hostname()
{
	if (api_hostname == NULL)
		return API_HOSTNAME;
	else
		return api_hostname;
}

void set_api_hostname(const char *hostname)
{
	k_free(api_hostname);
	api_hostname = k_malloc(strlen(hostname) + 1);
	if (api_hostname != NULL) {
		strcpy(api_hostname, hostname);
	}
}

uint16_t get_api_port()
{
	if (api_port == 0)
		return API_PORT;
	else
		return api_port;
}

void set_api_port(uint16_t port)
{
	api_port = port;
}

char *get_fw_api_hostname()
{
	if (fw_api_hostname == NULL)
		return FW_HOSTNAME;
	else
		return fw_api_hostname;
}

void set_fw_api_hostname(const char *hostname)
{
	k_free(fw_api_hostname);
	fw_api_hostname = k_malloc(strlen(hostname) + 1);
	if (fw_api_hostname != NULL) {
		strcpy(fw_api_hostname, hostname);
	}
}
