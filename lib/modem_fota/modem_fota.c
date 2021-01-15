/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>
#include <string.h>
#include <date_time.h>
#include <zephyr.h>
#include <bsd.h>
#include <logging/log.h>
#include <power/reboot.h>
#include <sys/timeutil.h>
#include <settings/settings.h>
#include <random/rand32.h>
#include <modem/at_cmd.h>
#include <modem/at_cmd_parser.h>
#include <modem/at_params.h>
#include <modem/at_notif.h>
#include <net/fota_download.h>
#include <modem/modem_fota.h>
#include <nrf_socket.h>
#include <dfu/dfu_target.h>
#include "modem_fota_internal.h"
#if defined CONFIG_MODEM_FOTA_CARRIER_LIB
#include "modem_fota_carrier.h"
#endif
#include "fota_client_mgmt.h"

/* TODO: +CEREG=5 and +CSCON=1 are enabled by LTE link control. The
 * documentation must state that either LTE link control must be used or these
 * notifications must be enabled by the application to use the FOTA library.
 */

LOG_MODULE_REGISTER(modem_fota, CONFIG_MODEM_FOTA_LOG_LEVEL);

/* HTTP fragment size needs to be limited, otherwise the received TLS record
 * will be too big for modem buffer.
 */
#define MODEM_FOTA_HTTP_FRAGMENT_SIZE 1792

/* Enums */
enum modem_reg_status {
	MODEM_REG_STATUS_NOT_REGISTERED,
	MODEM_REG_STATUS_HOME,
	MODEM_REG_STATUS_ROAMING
};

enum modem_lte_mode {
	MODEM_LTE_MODE_M = 7,
	MODEM_LTE_MODE_NBIOT = 9
};

/* Forward declarations */
static bool wait_for_data_inactivity(void);
static void restore_system_mode(void);
static void schedule_next_update(void);
static void update_check_timer_handler(struct k_timer *dummy);
static void active_time_timer_handler(struct k_timer *dummy);
static void at_notification_handler(void *context, const char *notif);

/* Work queue */
#define WORK_QUEUE_STACK_SIZE 2048
#define WORK_QUEUE_PRIORITY 5

K_THREAD_STACK_DEFINE(work_q_stack_area, WORK_QUEUE_STACK_SIZE);

static struct k_work_q work_q;
static struct k_work provision_work;
static struct k_work update_work;
static struct k_work update_job_status_work;
static struct k_work read_lte_active_time_work;
static struct k_work schedule_next_update_work;
static struct k_delayed_work retry_download_work;

struct update_info {
	struct k_work finish_update_work;
	enum modem_fota_evt_id event;
};

static struct update_info current_update_info;

static struct k_sem attach_sem;
static struct k_sem detach_sem;
static struct k_sem rrc_idle_sem;
static struct k_sem lte_active_sem;

K_TIMER_DEFINE(update_check_timer, update_check_timer_handler, NULL);
K_TIMER_DEFINE(active_time_timer, active_time_timer_handler, NULL);

#define AT_CEREG_REG_STATUS_INDEX	1
#define AT_CEREG_TAC_INDEX		2
#define AT_CEREG_TAC_LEN		4
#define AT_CEREG_LTE_MODE_INDEX		4
#define AT_CEREG_PARAMS_COUNT_MAX	(AT_CEREG_LTE_MODE_INDEX + 1)
#define AT_CSCON_MODE_INDEX		1
#define AT_CSCON_PARAMS_COUNT_MAX	(AT_CSCON_MODE_INDEX + 1)
#define AT_XTIME_UNIVERSAL_TIME_INDEX	2
#define AT_CGSN_IMEI_INDEX		1
#define AT_CGMR_VERSION_INDEX		0
#define AT_XSYSTEMMODE_PARAMS_COUNT	5
#define AT_XSYSTEMMODE_RESPONSE_MAX_LEN	30
#define AT_XMONITOR_ACTIVE_TIME_INDEX	14
#define AT_XMONITOR_ACTIVE_TIME_LEN	8
#define AT_XMONITOR_PARAMS_COUNT_MAX	(AT_XMONITOR_ACTIVE_TIME_INDEX + 1)
#define AT_XMONITOR_RESPONSE_MAX_LEN	128
#define AT_CCLK_TIME_LEN		20
#define AT_XMODEMUUID_UUID_LEN		36
#define AT_XAPNSTATUS_RESPONSE_MAX_LEN	256
#define AT_XAPNSTATUS_PARAMS_COUNT      4
#define AT_XAPNSTATUS_COMMAND_MAX_LEN   128
#define AT_CGDCONT_RESPONSE_MAX_LEN	128
#define AT_CGDCONT_PARAMS_COUNT		4
#define AT_CGDCONT_APN_INDEX            3
#define AT_CMNG_TYPE_ROOT_CA_CERT	0
#define AT_CMNG_TYPE_CLIENT_CERT	1

#define AT_XMONITOR_ACTIVE_TIME_UNIT_MASK	0xe0
#define AT_XMONITOR_ACTIVE_TIME_UNIT_2S		0x00
#define AT_XMONITOR_ACTIVE_TIME_UNIT_1MIN	0x20
#define AT_XMONITOR_ACTIVE_TIME_UNIT_6MIN	0x40
#define AT_XMONITOR_ACTIVE_TIME_DISABLED	0xe0

static const char at_cereg_notif[] = "+CEREG";
static const char at_cscon_notif[] = "+CSCON";
static const char at_xtime_notif[] = "\%XTIME";
static const char at_cfun_poweroff[] = "AT+CFUN=0";
static const char at_cfun_normal[] = "AT+CFUN=1";
static const char at_cfun_offline[] = "AT+CFUN=4";
static const char at_xtime_enable[] = "AT\%XTIME=1";
static const char at_cimi[] = "AT+CIMI";
static const char at_xsystemmode_read[] = "AT\%XSYSTEMMODE?";
static const char at_xsystemmode_template[] = "AT%%XSYSTEMMODE=%d,%d,%d,%d";
static const char at_xsystemmode_m1_only[] = "AT\%XSYSTEMMODE=1,0,0,0";
static const char at_xmonitor[] = "AT\%XMONITOR";
static const char at_xmodemuuid[] = "AT\%XMODEMUUID";
static const char at_xapnstatus_read[] = "AT\%XAPNSTATUS?";
static const char at_xapnstatus_template[] = "AT%%XAPNSTATUS=%d,\"%s\"";
static const char at_cgdcont_read[] = "AT+CGDCONT?";
static const char at_cmng_list_template[] = "AT%%CMNG=1,%d,%d";
static const char at_cmng_write_template[] = "AT%%CMNG=0,%d,%d,\"%s\"";
static const char job_status_queued[] = "QUEUED";
static const char job_status_in_progress[] = "IN PROGRESS";
static const char job_status_succeeded[] = "SUCCEEDED";
static const char job_status_failed[] = "FAILED";
static const char job_status_timed_out[] = "TIMED OUT";
static const char job_status_rejected[] = "REJECTED";
static const char job_status_downloading[] = "DOWNLOADING";
static const char job_status_canceled[] = "CANCELED";
static const char job_status_unknown[] = "UNKNOWN JOB STATUS";

/* AWS root CA certificate */
static const char root_ca_cert[] = {
	#include "cert/Amazon-Root-CA-1"
};

static modem_fota_callback_t event_callback;
char fota_device_id[MODEM_FOTA_DEVICE_ID_MAX_LEN + 1];

/* Modem time (milliseconds since epoch) and timestamp when it was got */
static int64_t modem_time;
static int64_t modem_time_timestamp;

/* Current modem status */
static enum modem_reg_status reg_status;
static enum modem_lte_mode lte_mode;
static bool rrc_idle = true;
static bool psm_enabled = false;
static bool lte_active = false;

/* FOTA is enabled by default */
static bool fota_enabled = true;

/* Current FOTA update job */
static struct fota_client_mgmt_job current_job;
static int download_retry_count;

/* Settings which are saved to NV memory */
/* Flag indicating if the device has been provisioned for FOTA */
static bool provisioning_done;
/* Next scheduled update check time (seconds since epoch) */
static int64_t update_check_time_s;
/* Modem build UUID before update */
static char *previous_modem_uuid;
/* Update job ID, used to update the job after reboot */
static char *update_job_id;

/* FOTA APN or NULL if default APN is used */
const char *fota_apn;
/* PDN socket file descriptor for FOTA PDN activation */
static int pdn_fd = -1;
static char *default_apn;

/* Information for restoring the system mode */
static bool restore_system_mode_needed = false;
static uint32_t prev_system_mode_bitmask = 0;

static bool restore_apn_status_needed = false;

static bool reboot_pending = false;

static bool provisioning_enabled = true;

static int settings_set(const char *name, size_t len,
		settings_read_cb read_cb, void *cb_arg)
{
	if (!strcmp(name, "provisioning_done")) {
		if (len != sizeof(provisioning_done))
			return -EINVAL;

		if (read_cb(cb_arg, &provisioning_done, len) > 0)
			return 0;
	} else if (!strcmp(name, "update_check_time")) {
		if (len != sizeof(update_check_time_s))
			return -EINVAL;

		if (read_cb(cb_arg, &update_check_time_s, len) > 0)
			return 0;
	} else if (!strcmp(name, "update_job_id")) {
		update_job_id = k_malloc(len);
		if (update_job_id == NULL)
			return -ENOMEM;

		if (read_cb(cb_arg, update_job_id, len) > 0) {
			return 0;
		} else {
			k_free(update_job_id);
			update_job_id = NULL;
			return -EINVAL;
		}
	} else if (!strcmp(name, "modem_uuid")) {
		if (len != AT_XMODEMUUID_UUID_LEN + 1)
			return -EINVAL;

		previous_modem_uuid = k_malloc(AT_XMODEMUUID_UUID_LEN + 1);
		if (previous_modem_uuid == NULL)
			return -ENOMEM;

		if (read_cb(cb_arg, previous_modem_uuid, len) > 0) {
			return 0;
		} else {
			k_free(previous_modem_uuid);
			previous_modem_uuid = NULL;
			return -EINVAL;
		}
	}

	return -ENOENT;
}

static void init_and_load_settings(void)
{
	settings_subsys_init();

	struct settings_handler my_conf = {
	    .name = "modem_fota",
	    .h_set = settings_set
	};

	settings_register(&my_conf);
	settings_load();
}

static void save_provisioning_done(void)
{
	settings_save_one("modem_fota/provisioning_done",
			  &provisioning_done, sizeof(provisioning_done));
}

static void save_update_check_time(void)
{
	settings_save_one("modem_fota/update_check_time",
			  &update_check_time_s, sizeof(update_check_time_s));
}

static void save_update_job_id(const char *job_id)
{
	if (job_id == NULL) {
		settings_delete("modem_fota/update_job_id");
	} else {
		settings_save_one("modem_fota/update_job_id",
				  job_id, strlen(job_id) + 1);
	}
}

static void save_modem_uuid(const char *uuid)
{
	if (uuid == NULL) {
		settings_delete("modem_fota/modem_uuid");
	} else {
		settings_save_one("modem_fota/modem_uuid",
				  uuid, AT_XMODEMUUID_UUID_LEN + 1);
	}
}

static void parse_xtime_time(const char *time_str)
{
	struct tm date_time;
	char temp[3] = {0};

	/* Year */
	temp[0] = time_str[1];
	temp[1] = time_str[0];
	date_time.tm_year = atoi(temp) + 2000 - 1900;

	/* Month */
	temp[0] = time_str[3];
	temp[1] = time_str[2];
	date_time.tm_mon = atoi(temp);
	/* Month is presented by months since January (0-11) */
	date_time.tm_mon--;

	/* Day */
	temp[0] = time_str[5];
	temp[1] = time_str[4];
	date_time.tm_mday = atoi(temp);

	/* Hour */
	temp[0] = time_str[7];
	temp[1] = time_str[6];
	date_time.tm_hour = atoi(temp);

	/* Minute */
	temp[0] = time_str[9];
	temp[1] = time_str[8];
	date_time.tm_min = atoi(temp);

	/* Second */
	temp[0] = time_str[11];
	temp[1] = time_str[10];
	date_time.tm_sec = atoi(temp);

	LOG_DBG("Time: %d.%d.%d %02d:%02d:%02d UTC",
		date_time.tm_mday, date_time.tm_mon + 1, date_time.tm_year + 1900,
		date_time.tm_hour, date_time.tm_min, date_time.tm_sec);

	modem_time = (int64_t)timeutil_timegm64(&date_time) * 1000;
	modem_time_timestamp = k_uptime_get();
}

static void parse_cclk_time(const char *time_str)
{
	struct tm date_time;
	char temp[3] = {0};

	/* Year */
	temp[0] = time_str[0];
	temp[1] = time_str[1];
	date_time.tm_year = atoi(temp) + 2000 - 1900;

	/* Month */
	temp[0] = time_str[3];
	temp[1] = time_str[4];
	date_time.tm_mon = atoi(temp);
	/* Month is presented by months since January (0-11) */
	date_time.tm_mon--;

	/* Day */
	temp[0] = time_str[6];
	temp[1] = time_str[7];
	date_time.tm_mday = atoi(temp);

	/* Hour */
	temp[0] = time_str[9];
	temp[1] = time_str[10];
	date_time.tm_hour = atoi(temp);

	/* Minute */
	temp[0] = time_str[12];
	temp[1] = time_str[13];
	date_time.tm_min = atoi(temp);

	/* Second */
	temp[0] = time_str[15];
	temp[1] = time_str[16];
	date_time.tm_sec = atoi(temp);

	LOG_DBG("Time: %d.%d.%d %02d:%02d:%02d UTC",
		date_time.tm_mday, date_time.tm_mon + 1, date_time.tm_year + 1900,
		date_time.tm_hour, date_time.tm_min, date_time.tm_sec);

	modem_time = (int64_t)timeutil_timegm64(&date_time) * 1000;
	modem_time_timestamp = k_uptime_get();
}

static bool is_modem_time_valid(void)
{
	return modem_time != 0 && modem_time_timestamp != 0;
}

static char *param_string_get(const char *str, int index)
{
	int err;
	char *param_str = NULL;
	size_t param_str_len;
	struct at_param_list param_list = {0};

	err = at_params_list_init(&param_list, index + 1);
	if (err) {
		LOG_ERR("Could not initialize params list, error: %d", err);
		return NULL;
	}

	err = at_parser_max_params_from_str(str,
					    NULL,
					    &param_list,
					    index + 1);
	if (err && err != -E2BIG) {
		LOG_ERR("Could not parse response, error: %d", err);
		goto clean_exit;
	}

	err = at_params_size_get(&param_list, index, &param_str_len);
	if (err) {
		LOG_ERR("Could not get parameter length, error: %d", err);
		goto clean_exit;
	}

	param_str = k_malloc(param_str_len + 1);

	err = at_params_string_get(&param_list,
				   index,
				   param_str,
				   &param_str_len);
	if (err) {
		LOG_ERR("Could not get parameter, error: %d", err);
		k_free(param_str);
		param_str = NULL;
		goto clean_exit;
	}

	param_str[param_str_len] = '\0';

clean_exit:
	at_params_list_free(&param_list);

	return param_str;
}

static int parse_cereg_notification(const char *notif)
{
	int err;
	uint32_t value;
	char tac_str[AT_CEREG_TAC_LEN + 1];
	size_t tac_str_len;
	bool tac_valid;
	struct at_param_list param_list = {0};

	err = at_params_list_init(&param_list, AT_CEREG_PARAMS_COUNT_MAX);
	if (err) {
		LOG_ERR("Could not initialize params list, error: %d", err);
		return err;
	}

	err = at_parser_max_params_from_str(notif,
					    NULL,
					    &param_list,
					    AT_CEREG_PARAMS_COUNT_MAX);
	if (err && err != -E2BIG) {
		LOG_ERR("Could not parse response, error: %d", err);
		goto clean_exit;
	}

	/* Registration status */
	err = at_params_int_get(&param_list,
				AT_CEREG_REG_STATUS_INDEX,
				&value);
	if (err) {
		LOG_ERR("Could not get registration status, error: %d", err);
		goto clean_exit;
	}

	/* Tracking area code */
	tac_str_len = AT_CEREG_TAC_LEN;
	err = at_params_string_get(&param_list,
				   AT_CEREG_TAC_INDEX,
				   tac_str,
				   &tac_str_len);
	if (err) {
		LOG_ERR("Could not get TAC, error: %d", err);
		goto clean_exit;
	}

	if (tac_str_len == AT_CEREG_TAC_LEN &&
			strncmp(tac_str, "FFFE", tac_str_len) != 0) {
		tac_valid = true;
	} else {
		tac_valid = false;
	}

	/* Need to be registered and have a valid TAC */
	if (value == 0) {
		reg_status = MODEM_REG_STATUS_NOT_REGISTERED;
		k_sem_give(&detach_sem);
	} else if (tac_valid && value == 1) {
		reg_status = MODEM_REG_STATUS_HOME;
		k_sem_give(&attach_sem);
	} else if (tac_valid && value == 5) {
		reg_status = MODEM_REG_STATUS_ROAMING;
		k_sem_give(&attach_sem);
	} else {
		reg_status = MODEM_REG_STATUS_NOT_REGISTERED;
	}

	/* LTE mode */
	err = at_params_int_get(&param_list,
				AT_CEREG_LTE_MODE_INDEX,
				&value);
	if (err) {
		LOG_ERR("Could not get LTE mode, error: %d", err);
		goto clean_exit;
	}

	if (value == 7)
		lte_mode = MODEM_LTE_MODE_M;
	else if (value == 9)
		lte_mode = MODEM_LTE_MODE_NBIOT;
	else
		LOG_WRN("Unknown LTE mode in +CEREG notification");

clean_exit:
	at_params_list_free(&param_list);

	return err;
}

static int parse_cscon_notification(const char *notif)
{
	int err;
	uint32_t value;
	struct at_param_list param_list = {0};

	err = at_params_list_init(&param_list, AT_CSCON_PARAMS_COUNT_MAX);
	if (err) {
		LOG_ERR("Could not initialize params list, error: %d", err);
		return err;
	}

	err = at_parser_max_params_from_str(notif,
					    NULL,
					    &param_list,
					    AT_CSCON_PARAMS_COUNT_MAX);
	if (err && err != -E2BIG ) {
		LOG_ERR("Could not parse response, error: %d", err);
		goto clean_exit;
	}

	/* Signaling connection mode */
	err = at_params_int_get(&param_list,
				AT_CSCON_MODE_INDEX,
				&value);
	if (err) {
		LOG_ERR("Could not get signaling connection mode, error: %d",
			err);
		goto clean_exit;
	}

	rrc_idle = value == 0 ? true : false;
	if (rrc_idle) {
		k_sem_give(&rrc_idle_sem);

		/* Schedule work which reads the LTE active time and starts
		 * a timer to determine when LTE enters PSM mode */
		k_work_submit(&read_lte_active_time_work);
	} else {
		k_timer_stop(&active_time_timer);
		lte_active = true;
		k_sem_give(&lte_active_sem);
	}

clean_exit:
	at_params_list_free(&param_list);

	return err;
}

static int parse_time_from_xtime_notification(const char *notif)
{
	char *time_str;

	time_str = param_string_get(notif, AT_XTIME_UNIVERSAL_TIME_INDEX);
	if (time_str != NULL) {
		parse_xtime_time(time_str);
		k_free(time_str);
		return 0;
	} else {
		return -1;
	}
}

static int64_t get_current_time_in_s(void)
{
	return (k_uptime_get() - modem_time_timestamp + modem_time) / 1000;
}

static void free_default_apn_name(void)
{
	k_free(default_apn);
	default_apn = NULL;
}

static int read_default_apn_name(void)
{
	int err = 0;
	char response[AT_CGDCONT_RESPONSE_MAX_LEN];
	char apn[AT_CGDCONT_RESPONSE_MAX_LEN];
	size_t apn_len = AT_CGDCONT_RESPONSE_MAX_LEN;
	struct at_param_list param_list = {0};

	/* Clear the previous APN */
	free_default_apn_name();

	/* Read the default APN and store it */

	err = at_cmd_write(at_cgdcont_read,
			   response,
			   AT_CGDCONT_RESPONSE_MAX_LEN,
			   NULL);
	if (err) {
		LOG_ERR("Failed to read context status, error: %d", err);
		return false;
	}

	err = at_params_list_init(&param_list, AT_CGDCONT_PARAMS_COUNT);
	if (err) {
		LOG_ERR("Could not init AT params list, error: %d", err);
		return false;
	}

	err = at_parser_max_params_from_str(response, NULL, &param_list,
					    AT_CGDCONT_PARAMS_COUNT);
	if (err && err != -E2BIG && err != -EAGAIN) {
		LOG_ERR("Could not parse AT response, error: %d", err);
		goto clean_exit;
	}

	err = at_params_string_get(&param_list, AT_CGDCONT_APN_INDEX,
				   apn, &apn_len);
	if (err) {
		LOG_ERR("Failed to get string, error: %d", err);
		goto clean_exit;
	}

	if (apn_len > 0) {
		default_apn = k_malloc(apn_len + 1);
		if (default_apn == NULL) {
			err = -ENOMEM;
			goto clean_exit;
		}
		strncpy(default_apn, apn, apn_len);
		default_apn[apn_len] = '\0';
	}

clean_exit:
	at_params_list_free(&param_list);

	return err;
}

static bool is_apn_disabled(const char *apn)
{
	int err;
	char response[AT_XAPNSTATUS_RESPONSE_MAX_LEN];
	char current_apn[AT_XAPNSTATUS_COMMAND_MAX_LEN];
	size_t current_apn_len;
	struct at_param_list param_list = {0};
	bool apn_disabled = false;
	int param_count;

	err = at_cmd_write(at_xapnstatus_read,
			   response,
			   AT_XAPNSTATUS_RESPONSE_MAX_LEN,
			   NULL);
	if (err) {
		LOG_ERR("Failed to read APN status, error: %d", err);
		return false;
	}

	err = at_params_list_init(&param_list, AT_XAPNSTATUS_PARAMS_COUNT);
	if (err) {
		LOG_ERR("Could not init AT params list, error: %d", err);
		return false;
	}

	err = at_parser_max_params_from_str(response, NULL, &param_list,
					    AT_XAPNSTATUS_PARAMS_COUNT);
	if (err && err != -EAGAIN) {
		LOG_ERR("Could not parse AT response, error: %d", err);
		goto clean_exit;
	}

	/* Check if the list of disabled APNs contains the FOTA APN */
	param_count = at_params_valid_count_get(&param_list);
	for (int i = 1; i < param_count; i++) {
		current_apn_len = sizeof(current_apn);
		err = at_params_string_get(&param_list, i,
					   current_apn, &current_apn_len);
		if (err) {
			LOG_ERR("Failed to get string, error: %d", err);
			goto clean_exit;
		}
		if (strlen(apn) == current_apn_len &&
		    strncmp(apn, current_apn, current_apn_len) == 0) {
			apn_disabled = true;
			break;
		}
	}

clean_exit:
	at_params_list_free(&param_list);

	return apn_disabled;
}

static void send_xapnstatus_command(bool enable, const char *apn)
{
	int err;
	char command[AT_XAPNSTATUS_COMMAND_MAX_LEN];

	snprintk(command,
		 sizeof(command),
		 at_xapnstatus_template,
		 enable ? 1 : 0,
		 apn);
	command[AT_XAPNSTATUS_COMMAND_MAX_LEN - 1] = '\0';
	err = at_cmd_write(command, NULL, 0, NULL);
	if (err) {
		LOG_ERR("Failed to set APN status, error: %d", err);
	}
}

static void enable_apn(const char *apn)
{
	send_xapnstatus_command(true, apn);
}

static void disable_apn(const char *apn)
{
	send_xapnstatus_command(false, apn);
}

static int activate_fota_pdn(void)
{
	int err;
	nrf_sa_family_t af[2];
	int af_count;

	if (fota_apn == NULL) {
		/* No separate FOTA APN is configured, check if default APN
		 * usage has been disabled and temporarily enable APN usage if
		 * necessary.
		 */
		err = read_default_apn_name();
		if (err == 0 && default_apn != NULL) {
			if (is_apn_disabled(default_apn)) {
				LOG_INF("Default APN usage disabled, enabling "
					"APN temporarily");
				enable_apn(default_apn);
				restore_apn_status_needed = true;
			} else {
				free_default_apn_name();
			}
		}

		return 0;
	}

	LOG_INF("Activating FOTA PDN if necessary");

	/* Check if FOTA APN usage has been disabled and temporarily enable
	 * APN usage if necessary.
	 */
	if (is_apn_disabled(fota_apn)) {
		LOG_INF("FOTA APN usage disabled, enabling APN temporarily");
		enable_apn(fota_apn);
		restore_apn_status_needed = true;
	}

	pdn_fd = nrf_socket(NRF_AF_LTE, NRF_SOCK_MGMT, NRF_PROTO_PDN);
	if (pdn_fd < 0) {
		LOG_ERR("Failed to open PDN socket");
		return -1;
	}

#if defined(CONFIG_MODEM_FOTA_APN_AUTH_PAP) || defined(CONFIG_MODEM_FOTA_APN_AUTH_CHAP)
	nrf_pdn_auth_t auth_params;

#if !defined CONFIG_MODEM_FOTA_CARRIER_LIB
	strncpy(auth_params.username,
		CONFIG_MODEM_FOTA_APN_AUTH_USERNAME,
		NRF_PDN_MAX_USERNAME_LEN);
	strncpy(auth_params.password,
		CONFIG_MODEM_FOTA_APN_AUTH_PASSWORD,
		NRF_PDN_MAX_PASSWORD_LEN);
#else
	char *temp_str;

	temp_str = modem_fota_carrier_get_apn_username();
	if (temp_str == NULL) {
		LOG_ERR("Failed to get APN username");
		return -1;
	}
	strncpy(auth_params.username,
		temp_str,
		NRF_PDN_MAX_USERNAME_LEN);
	k_free(temp_str);

	temp_str = modem_fota_carrier_get_apn_password();
	if (temp_str == NULL) {
		LOG_ERR("Failed to get APN password");
		return -1;
	}
	strncpy(auth_params.password,
		temp_str,
		NRF_PDN_MAX_PASSWORD_LEN);
	k_free(temp_str);
#endif
	auth_params.username[NRF_PDN_MAX_USERNAME_LEN - 1] = '\0';
	auth_params.password[NRF_PDN_MAX_PASSWORD_LEN - 1] = '\0';

	if (IS_ENABLED(CONFIG_MODEM_FOTA_APN_AUTH_PAP)) {
		auth_params.authentication_type = NRF_PDN_AUTH_TYPE_PAP;
	} else if (IS_ENABLED(CONFIG_MODEM_FOTA_APN_AUTH_CHAP)) {
		auth_params.authentication_type = NRF_PDN_AUTH_TYPE_CHAP;
	} else {
		/* Unknown authentication type */
		auth_params.authentication_type = NRF_PDN_AUTH_TYPE_NONE;
	}

	err = nrf_setsockopt(pdn_fd, NRF_SOL_PDN, NRF_SO_PDN_AUTH,
			     &auth_params, sizeof(auth_params));
	if (err) {
		LOG_ERR("Could not set PDN authentication parameters, "
			"error: %d", err);
		return err;
	}
#endif

	/* Configure PDN type (IPv4/IPv6/IPv4v6) */
	af_count = 0;
	if (IS_ENABLED(CONFIG_MODEM_FOTA_APN_PDN_TYPE_IPV4)) {
		LOG_DBG("FOTA PDN type IPv4");
		af[af_count++] = NRF_AF_INET;
	} else if (IS_ENABLED(CONFIG_MODEM_FOTA_APN_PDN_TYPE_IPV6)) {
		LOG_DBG("FOTA PDN type IPv6");
		af[af_count++] = NRF_AF_INET6;
	} else {
		LOG_DBG("FOTA PDN type IPv4v6");
		af[af_count++] = NRF_AF_INET;
		af[af_count++] = NRF_AF_INET6;
	}
	err = nrf_setsockopt(pdn_fd, NRF_SOL_PDN, NRF_SO_PDN_AF,
			     af, sizeof(nrf_sa_family_t) * af_count);
	if (err) {
		LOG_ERR("Could not set FOTA PDN type, error: %d", err);
		return err;
	}

	err = nrf_connect(pdn_fd, fota_apn, strlen(fota_apn));
	if (err) {
		LOG_ERR("Could not connect FOTA PDN, error: %d", err);
		return err;
	}

	return 0;
}

static void deactivate_fota_pdn(void)
{
	if (pdn_fd >= 0) {
		LOG_INF("Deactivating FOTA PDN if necessary");

		nrf_close(pdn_fd);
		pdn_fd = -1;
	}

	/* Disable APN usage if it was temporarily enabled */
	if (restore_apn_status_needed) {
		if (fota_apn != NULL) {
			LOG_INF("Disabling FOTA APN usage");
			disable_apn(fota_apn);
		} else if (default_apn != NULL) {
			LOG_INF("Disabling default APN usage");
			disable_apn(default_apn);
			free_default_apn_name();
		}
		restore_apn_status_needed = false;
	}
}

void dfu_target_callback_handler(enum dfu_target_evt_id evt_id)
{
	/* Nothing to do here */
}

static void erase_modem_fw_backup(void)
{
	int fd;
	int err;
	nrf_dfu_fw_offset_t offset;
	nrf_socklen_t len = sizeof(offset);

	LOG_INF("Erasing modem FW backup...");

	fd = nrf_socket(NRF_AF_LOCAL, NRF_SOCK_STREAM, NRF_PROTO_DFU);
	if (fd < 0) {
		LOG_ERR("Failed to open modem DFU socket");
		return;
	}

	err = nrf_setsockopt(fd, NRF_SOL_DFU, NRF_SO_DFU_BACKUP_DELETE, NULL, 0);
	if (err < 0) {
		LOG_ERR("Failed to erase modem FW backup, errno: %d", errno);
		nrf_close(fd);
		return;
	}
	while (true) {
		err = nrf_getsockopt(fd, NRF_SOL_DFU, NRF_SO_DFU_OFFSET, &offset, &len);
		if (err < 0) {
			k_sleep(K_SECONDS(1));
		} else {
			LOG_INF("Modem FW backup erase completed");
			break;
		}
	}

	nrf_close(fd);
}

static void schedule_modem_fw_revert(void)
{
	int fd;
	int err;

	LOG_WRN("Scheduling modem FW revert on next boot...");

	fd = nrf_socket(NRF_AF_LOCAL, NRF_SOCK_STREAM, NRF_PROTO_DFU);
	if (fd < 0) {
		LOG_ERR("Failed to open modem DFU socket");
		return;
	}

	err = nrf_setsockopt(fd, NRF_SOL_DFU, NRF_SO_DFU_REVERT, NULL, 0);
	if (err < 0) {
		LOG_ERR("Failed to schedule modem FW revert, errno: %d", errno);
	}

	nrf_close(fd);
}

static int get_modem_uuid(char *uuid_buf)
{
	int err;
	char uuid_str[64];
	char *uuid_ptr;

	err = at_cmd_write(at_xmodemuuid, uuid_str, sizeof(uuid_str), NULL);
	if (err) {
		return err;
	}

	/* Remove command name and space from output */
	uuid_ptr = strchr(uuid_str, ' ');
	if (uuid_ptr == NULL) {
		return -EINVAL;
	}
	uuid_ptr++;

	/* Check if UUID is a quoted string */
	if (*uuid_ptr == '\"') {
		uuid_ptr++;
	}

	strncpy(uuid_buf, uuid_ptr, AT_XMODEMUUID_UUID_LEN);
	uuid_buf[AT_XMODEMUUID_UUID_LEN] = '\0';

	return 0;
}

static int get_pending_job(void)
{
	int ret;
	int retry_count;
	uint32_t start_time;
	int32_t wait_time;

	fota_client_job_free(&current_job);

	retry_count = CONFIG_MODEM_FOTA_SERVER_RETRY_COUNT;
	while (true) {
		LOG_INF("Checking for FOTA update...");

		start_time = k_uptime_get_32();

		ret = fota_client_get_pending_job(&current_job);
		if (ret == 0 || retry_count <= 0) {
			/* Check successful or no retries left */
			break;
		}

		LOG_WRN("Checking for FOTA update failed. %d retries left...",
			retry_count);
		retry_count--;

		/* Make sure retries have at least 30s interval */
		wait_time = 30 * 1000 - (k_uptime_get_32() - start_time);
		if (wait_time > 0) {
			k_sleep(K_MSEC(wait_time));
		}
	}

	if (ret == 0) {
		if (current_job.host) {
			LOG_INF("FOTA update job is available");
			LOG_INF("ID: %s", log_strdup(current_job.id));
			LOG_DBG("Host: %s", log_strdup(current_job.host));
			LOG_DBG("Path: %s", log_strdup(current_job.path));
		} else {
			LOG_INF("No FOTA update available");
		}
	} else {
		LOG_ERR("Failed to check for FOTA update, error: %d", ret);
	}

	return ret;
}

static const char *get_job_status_string(enum nrf_cloud_fota_status status) {
	switch (status) {
	case NRF_CLOUD_FOTA_QUEUED:
		return job_status_queued;
	case NRF_CLOUD_FOTA_IN_PROGRESS:
		return job_status_in_progress;
	case NRF_CLOUD_FOTA_SUCCEEDED:
		return job_status_succeeded;
	case NRF_CLOUD_FOTA_FAILED:
		return job_status_failed;
	case NRF_CLOUD_FOTA_TIMED_OUT:
		return job_status_timed_out;
	case NRF_CLOUD_FOTA_REJECTED:
		return job_status_rejected;
	case NRF_CLOUD_FOTA_DOWNLOADING:
		return job_status_downloading;
	case NRF_CLOUD_FOTA_CANCELED:
		return job_status_canceled;
	default:
		LOG_ERR("Unknown job status");
		return job_status_unknown;
	}
}

static int update_job_status(void)
{
	int ret;
	int retry_count;
	uint32_t start_time;
	int32_t wait_time;

	retry_count = CONFIG_MODEM_FOTA_SERVER_RETRY_COUNT;
	while (true) {
		LOG_INF("Updating FOTA update job status to %s...",
			get_job_status_string(current_job.status));

		start_time = k_uptime_get_32();

		ret = fota_client_update_job(&current_job);
		if (ret == 0 || retry_count <= 0) {
			/* Check successful or no retries left */
			break;
		}

		LOG_WRN("Updating job failed. %d retries left...",
			retry_count);
		retry_count--;

		/* Make sure retries have at least 30s interval */
		wait_time = 30 * 1000 - (k_uptime_get_32() - start_time);
		if (wait_time > 0) {
			k_sleep(K_MSEC(wait_time));
		}
	}

	if (ret == 0) {
		LOG_INF("Job status updated");
	} else {
		LOG_ERR("Failed to update job status, error: %d\n", ret);
	}

	return ret;
}

static void finish_update(enum modem_fota_evt_id event_id)
{
	current_update_info.event = event_id;
	k_work_submit_to_queue(&work_q, &current_update_info.finish_update_work);
}

/* Waits until the device is detached from the network or a timeout happens.
 * The return value indicates if device is detached from the network or not.
 * Timeout is given in seconds, zero means "no wait", i.e. function returns the
 * network connection status immediately.
 */
static bool send_at_command_and_wait_until_detached(const char *at_cmd,
						    uint32_t timeout_s)
{
	if (reg_status == MODEM_REG_STATUS_HOME ||
	    reg_status == MODEM_REG_STATUS_ROAMING) {

		k_sem_reset(&detach_sem);

		at_cmd_write(at_cmd, NULL, 0, NULL);
		if (timeout_s == 0) {
			return false;
		}

		LOG_INF("Waiting for network detach (timeout %d s)...",
			timeout_s);
		if (k_sem_take(&detach_sem, K_SECONDS(timeout_s)) != 0) {
			return false;
		}
	}

	return true;
}

static void reboot_to_apply_update(void)
{
	LOG_INF("Rebooting to apply modem firmware update...");

	send_at_command_and_wait_until_detached(at_cfun_poweroff, 30);

	k_sleep(K_SECONDS(5));

	sys_reboot(SYS_REBOOT_WARM);
}

/* Performs cleanup after FW update check or FW update and schedules the next
 * update check.
 */
static void finish_update_work_fn(struct k_work *item)
{
	int err;
	struct update_info *info;
	char modem_uuid[AT_XMODEMUUID_UUID_LEN + 1];
	bool reboot_now = false;

	info = CONTAINER_OF(item, struct update_info, finish_update_work);

	switch (info->event) {
	case MODEM_FOTA_EVT_UPDATE_DOWNLOADED:
		/* Save update job ID to NV so that the job can be updated
		 * after reboot.
		 */
		save_update_job_id(current_job.id);
		/* Save current modem UUID so that we can check if modem was
		 * really updated or not.
		 */
		err = get_modem_uuid(modem_uuid);
		if (err == 0) {
			save_modem_uuid(modem_uuid);
		}

		/* If modem firmware update was downloaded, a reboot is needed
		 * to apply the update.
		 */
		reboot_now = true;
		break;

	case MODEM_FOTA_EVT_ERROR:
		if (current_job.status == NRF_CLOUD_FOTA_REJECTED) {
			/* No point in retrying a rejected job */
			update_job_status();
			erase_modem_fw_backup();
		}
		break;

	default:
		/* No update available */
		break;
	}

	fota_client_job_free(&current_job);

	deactivate_fota_pdn();
	restore_system_mode();

	event_callback(info->event);

	/* Wait for data connection to become inactive before rebooting */
	if (reboot_now && !wait_for_data_inactivity()) {
		/* Timed out while waiting for inactivity, the system will be
		 * rebooted when the next update check is scheduled.
		 */
		LOG_INF("Reboot will be tried again later");

		reboot_now = false;
		reboot_pending = true;
	}

	schedule_next_update();

	if (reboot_now) {
		reboot_to_apply_update();
	}
}

static void fota_download_callback(const struct fota_download_evt *evt)
{
	switch (evt->id) {
	case FOTA_DOWNLOAD_EVT_FINISHED:
		LOG_INF("Update downloaded, reboot needed to apply update");
		finish_update(MODEM_FOTA_EVT_UPDATE_DOWNLOADED);
		break;

	case FOTA_DOWNLOAD_EVT_ERASE_PENDING:
		LOG_DBG("Modem scratch area erase pending...");
		break;

	case FOTA_DOWNLOAD_EVT_ERASE_DONE:
		LOG_DBG("Modem scratch area erase done");
		break;

	case FOTA_DOWNLOAD_EVT_ERROR:
		if (evt->cause == FOTA_DOWNLOAD_ERROR_CAUSE_INVALID_UPDATE) {
			LOG_ERR("Modem rejected the firmware");
			current_job.status = NRF_CLOUD_FOTA_REJECTED;
		} else {
			/* Download failed, check if we should retry */
			if (download_retry_count > 0) {
				LOG_WRN("Downloading the firmware failed. %d "
					"retries left...", download_retry_count);
				download_retry_count--;
				k_delayed_work_submit_to_queue(
						&work_q,
						&retry_download_work,
						K_SECONDS(30));
				return;
			}

			LOG_ERR("Downloading the firmware failed");
			current_job.status = NRF_CLOUD_FOTA_FAILED;
		}

		finish_update(MODEM_FOTA_EVT_ERROR);
		break;

	default:
		LOG_ERR("Unknown event from FOTA download");
		break;
	}
}

static bool is_update_check_allowed(void)
{
	if (!IS_ENABLED(CONFIG_MODEM_FOTA_ALLOWED_DURING_ROAMING) &&
	    reg_status == MODEM_REG_STATUS_ROAMING) {
		LOG_DBG("Roaming, update check not allowed");
		return false;
	}

	return true;
}

/* Waits until the device is attached to the network or a timeout happens.
 * The return value indicates if device is attached to the network or not. The
 * timeout is given in seconds, zero means "no wait", i.e. function returns the
 * network connection status immediately.
 */
static bool wait_until_attached(uint32_t timeout_s)
{
	if (reg_status != MODEM_REG_STATUS_HOME &&
	    reg_status != MODEM_REG_STATUS_ROAMING) {
		if (timeout_s == 0) {
			return false;
		}

		LOG_INF("Waiting for network attach (timeout %d s)...",
			timeout_s);
		k_sem_reset(&attach_sem);
		if (k_sem_take(&attach_sem, K_SECONDS(timeout_s)) != 0) {
			return false;
		}
	}

	return true;
}

/* Waits until the RRC connection is idle. This is used to wait until the
 * application is not transferring data before proceeding with the FW update.
 */
static bool wait_for_data_inactivity(void)
{
	uint32_t timeout_s;

	if (!rrc_idle) {
		timeout_s = CONFIG_MODEM_FOTA_DATA_INACTIVITY_TIMEOUT * 60;

		LOG_INF("Waiting for RRC connection release (timeout %d s)...",
			timeout_s);
		k_sem_reset(&rrc_idle_sem);
		if (k_sem_take(&rrc_idle_sem, K_SECONDS(timeout_s)) != 0) {
			return false;
		}
	}

	return true;
}

static bool switch_system_mode_to_lte_m(void)
{
	int err;
	bool success = true;
	char response[AT_XSYSTEMMODE_RESPONSE_MAX_LEN];
	struct at_param_list param_list = {0};

	/* Get and store system mode */
	err = at_cmd_write(at_xsystemmode_read, response, sizeof(response),
			   NULL);
	if (err) {
		LOG_ERR("Failed to read system mode, error: %d", err);
		return false;
	}

	err = at_params_list_init(&param_list, AT_XSYSTEMMODE_PARAMS_COUNT);
	if (err) {
		LOG_ERR("Could not init AT params list, error: %d", err);
		return false;
	}

	err = at_parser_max_params_from_str(response, NULL, &param_list,
					    AT_XSYSTEMMODE_PARAMS_COUNT);
	if (err) {
		LOG_ERR("Could not parse AT response, error: %d", err);
		goto clean_exit;
	}

	/* Current system mode is stored into a bitmap which is used when
	 * restoring the system mode later
	 */
	prev_system_mode_bitmask = 0;
	for (size_t i = 1; i < AT_XSYSTEMMODE_PARAMS_COUNT; i++) {
		int param;

		err = at_params_int_get(&param_list, i, &param);
		if (err) {
			LOG_ERR("Could not parse mode parameter, err: %d", err);
			goto clean_exit;
		}

		prev_system_mode_bitmask = param ?
			prev_system_mode_bitmask | BIT(i) : prev_system_mode_bitmask;
	}

	/* Set modem offline */
	if (!send_at_command_and_wait_until_detached(at_cfun_offline, 30)) {
		LOG_ERR("Failed to set modem to offline mode");
		return false;
	}

	LOG_INF("Changing system mode to LTE-M");

	restore_system_mode_needed = true;

	/* Change system mode to LTE-M only */
	err = at_cmd_write(at_xsystemmode_m1_only, NULL, 0, NULL);
	if (err) {
		LOG_ERR("Failed to set system mode, error: %d", err);
		success = false;
	}

	/* Set modem online */
	err = at_cmd_write(at_cfun_normal, NULL, 0, NULL);
	if (err) {
		LOG_ERR("Failed to set modem to normal mode, error: %d", err);
		success = false;
	}

	if (success) {
		/* Wait until connected to network or a timeout occurs */
		if (!wait_until_attached(300)) {
			LOG_ERR("Could not connect to LTE-M");
			success = false;
		}
	}

clean_exit:
	at_params_list_free(&param_list);

	return success;
}

static void restore_system_mode(void)
{
	int err;
	int len;
	char system_mode_command[32];

	if (!restore_system_mode_needed) {
		return;
	}

	LOG_DBG("Previous system mode bitmask: 0x%x", prev_system_mode_bitmask);
	LOG_INF("Restoring previous system mode");

	/* Set modem offline */
	if (!send_at_command_and_wait_until_detached(at_cfun_offline, 30)) {
		LOG_ERR("Failed to set modem to offline mode");
		return;
	}

	/* Restore system mode */
	len = snprintk(system_mode_command, sizeof(system_mode_command),
		       at_xsystemmode_template,
		       prev_system_mode_bitmask & BIT(1) ? 1 : 0,
		       prev_system_mode_bitmask & BIT(2) ? 1 : 0,
		       prev_system_mode_bitmask & BIT(3) ? 1 : 0,
		       prev_system_mode_bitmask & BIT(4) ? 1 : 0);

	err = at_cmd_write(system_mode_command, NULL, 0, NULL);
	if (err)
		LOG_ERR("Failed to set system mode, error: %d", err);

	/* Set modem online */
	err = at_cmd_write(at_cfun_normal, NULL, 0, NULL);
	if (err)
		LOG_ERR("Failed to set modem to normal mode, error: %d", err);

	LOG_INF("System mode restored");

	restore_system_mode_needed = false;
}

static void start_update_check(void)
{
	k_work_submit_to_queue(&work_q, &update_work);
}

static void clear_update_check_time(void)
{
	update_check_time_s = 0;
	save_update_check_time();
}

static int start_firmware_download_with_retry()
{
	int err;
	uint32_t start_time;
	int32_t wait_time;

	while (true) {
		LOG_INF("Starting firmware download...");

		start_time = k_uptime_get_32();

		err = fota_download_start(current_job.host,
					  current_job.path,
					  CONFIG_MODEM_FOTA_TLS_SECURITY_TAG,
					  fota_apn,
					  MODEM_FOTA_HTTP_FRAGMENT_SIZE);
		if (err == 0 || download_retry_count <= 0) {
			/* Download started successfully or no retries
			 * left
			 */
			break;
		}

		LOG_WRN("Starting firmware download failed. %d retries "
			"left...", download_retry_count);
		download_retry_count--;

		/* Make sure retries have at least 30s interval */
		wait_time = 30 * 1000 - (k_uptime_get_32() - start_time);
		if (wait_time > 0) {
			k_sleep(K_MSEC(wait_time));
		}
	}

	return err;
}

static void start_update_work_fn(struct k_work *item)
{
	int err;

	/* If in NB-IoT and RRC connection is active, skip this check */
	if (lte_mode == MODEM_LTE_MODE_NBIOT &&
	    !rrc_idle &&
	    IS_ENABLED(CONFIG_MODEM_FOTA_UPDATE_CHECK_IN_NBIOT_POSTPONED_BY_DATA_ACTIVITY)) {
		LOG_INF("NB-IoT and RRC connection active, skip update check");
		clear_update_check_time();
		schedule_next_update();
		return;
	}

	/* If in PSM, delay update check until modem wakes up */
	if (psm_enabled &&
	    IS_ENABLED(CONFIG_MODEM_FOTA_UPDATE_CHECK_BLOCKED_BY_PSM) &&
	    !lte_active) {
		LOG_INF("Waiting for modem to wake up from PSM...");
		k_sem_reset(&lte_active_sem);
		/* In theory PSM period could be days, so this might take
		 * long.
		 */
		k_sem_take(&lte_active_sem, K_FOREVER);
	}

	/* Clear the stored update check time to prevent unwanted update check
	 * if device reboots before the next update has been scheduled.
	 */
	clear_update_check_time();

	if (provisioning_enabled && !provisioning_done) {
		/* Provisioning has failed earlier, retry */
		k_work_submit_to_queue(&work_q, &provision_work);
		return;
	}

	if (reboot_pending) {
		LOG_INF("Update has already been downloaded, reboot needed");

		if (!wait_for_data_inactivity()) {
			/* Timed out while waiting for inactivity, the system
			 * will be rebooted when the next update check is
			 * scheduled. */
			LOG_INF("Reboot will be tried again later");
			schedule_next_update();
			return;
		}

		schedule_next_update();
		reboot_to_apply_update();
	}

	LOG_INF("Time for update check");

	if (!wait_until_attached(0)) {
		LOG_INF("Out of service, skip update check");
		schedule_next_update();
		return;
	}

	if (!is_update_check_allowed()) {
		LOG_INF("Update check not allowed");
		schedule_next_update();
		return;
	}

	/* If FOTA is not allowed in NB-IoT and the current mode is NB-IoT,
	 * we need to switch to M1 for the update check
	 */
	if (!IS_ENABLED(CONFIG_MODEM_FOTA_ALLOWED_IN_NBIOT)) {
		if (lte_mode == MODEM_LTE_MODE_NBIOT) {
			LOG_INF("FOTA not allowed in NB-IoT, switching to "
				"LTE-M");
			if (!wait_for_data_inactivity()) {
				/* Timed out, cancel check and schedule next
				 * update check */
				LOG_INF("Timed out, skip update check");
				schedule_next_update();
				return;
			}
			if (!switch_system_mode_to_lte_m()) {
				/* Failed to connect to LTE-M, abort update
				 * check
				 */
				restore_system_mode();
				schedule_next_update();
				return;
			}
		}
	}

	event_callback(MODEM_FOTA_EVT_CHECKING_FOR_UPDATE);

	err = activate_fota_pdn();
	if (err) {
		LOG_ERR("Activating FOTA PDN failed");
		finish_update(MODEM_FOTA_EVT_ERROR);
		return;
	}

	err = get_pending_job();
	if (err) {
		finish_update(MODEM_FOTA_EVT_ERROR);
		return;
	}

	if (current_job.host != NULL &&	current_job.path != NULL) {
		/* Initialize retry count before first download attempt */
		download_retry_count = CONFIG_MODEM_FOTA_SERVER_RETRY_COUNT;

		err = start_firmware_download_with_retry();

		if (err) {
			LOG_ERR("Downloading the firmware failed");
			finish_update(MODEM_FOTA_EVT_ERROR);
		}
	} else {
		finish_update(MODEM_FOTA_EVT_NO_UPDATE_AVAILABLE);
	}
}

static void retry_download_work_fn(struct k_work *item)
{
	int err;

	err = start_firmware_download_with_retry();

	if (err) {
		LOG_ERR("Downloading the firmware failed");
		finish_update(MODEM_FOTA_EVT_ERROR);
	}
}

static void update_job_status_after_apply(void)
{
	k_work_submit_to_queue(&work_q, &update_job_status_work);
}

static bool modem_fw_updated(void)
{
	int err;
	char modem_uuid[AT_XMODEMUUID_UUID_LEN + 1];

	if (previous_modem_uuid == NULL) {
		/* UUID check not possible */
		return false;
	}
	LOG_DBG("Previous modem UUID: %s", log_strdup(previous_modem_uuid));

	err = get_modem_uuid(modem_uuid);
	if (err) {
		/* UUID check not possible */
		return false;
	}
	LOG_DBG("Current modem UUID: %s", log_strdup(modem_uuid));

	if (strncmp(previous_modem_uuid, modem_uuid, AT_XMODEMUUID_UUID_LEN) != 0) {
		/* UUID has changed, modem FW has been updated */
		return true;
	}

	return false;
}

/* Updates the FOTA job document after the modem has been updated. Failures
 * in this function are considered to be caused by the new modem firmware and
 * trigger the modem firmware to be reverted.
 */
static void update_job_status_work_fn(struct k_work *item)
{
	int err;
	bool update_successful;

	update_successful = modem_fw_updated();

	if (update_successful) {
		LOG_INF("Modem firmware was updated, updating job");
	} else {
		LOG_WRN("Modem firmware was rejected, updating job");
	}

	if (!wait_until_attached(3600)) {
		LOG_ERR("Connecting to LTE failed");

		if (IS_ENABLED(CONFIG_MODEM_FOTA_FIRMWARE_SANITY_CHECK) &&
				update_successful) {
			/* Assuming the new MFW is faulty. Reverting back to
			 * previous version. */
			schedule_modem_fw_revert();
			/* This call reboots the device before returning. */
			reboot_to_apply_update();
		}

		LOG_ERR("Failed to update job status");
		goto clean_exit;
	}

	/* If FOTA is not allowed in NB-IoT and the current mode is NB-IoT,
	 * we need to switch to M1 for the job update
	 */
	if (!IS_ENABLED(CONFIG_MODEM_FOTA_ALLOWED_IN_NBIOT) &&
			lte_mode == MODEM_LTE_MODE_NBIOT) {
		LOG_INF("Switching to LTE-M for job update");
		/* Wait for inactivity, but ignore possible timeout */
		wait_for_data_inactivity();
		if (!switch_system_mode_to_lte_m()) {
			LOG_ERR("Switching to LTE-M failed");
			restore_system_mode();

			if (IS_ENABLED(CONFIG_MODEM_FOTA_FIRMWARE_SANITY_CHECK) &&
					update_successful) {
				/* Assuming the new MFW is faulty.
				 * Reverting back to previous
				 * version. */
				schedule_modem_fw_revert();
				/* This call reboots the device before
				 * returning.
				 */
				reboot_to_apply_update();
			}

			LOG_ERR("Failed to update job status");
			goto clean_exit;
		}
	}

	err = activate_fota_pdn();
	if (err) {
		LOG_ERR("Activating FOTA PDN failed");
		restore_system_mode();

		if (IS_ENABLED(CONFIG_MODEM_FOTA_FIRMWARE_SANITY_CHECK) &&
				update_successful) {
			/* Assuming the new MFW is faulty. Reverting back to
			 * previous version. */
			schedule_modem_fw_revert();
			/* This call reboots the device before returning. */
			reboot_to_apply_update();
		}

		LOG_ERR("Failed to update job status");
		goto clean_exit;
	}

	/* Update job status to server */
	current_job.id = update_job_id;
	update_job_id = NULL;
	if (update_successful) {
		current_job.status = NRF_CLOUD_FOTA_SUCCEEDED;
	} else {
		current_job.status = NRF_CLOUD_FOTA_REJECTED;
	}
	err = update_job_status();
	if (err) {
		if (IS_ENABLED(CONFIG_MODEM_FOTA_FIRMWARE_SANITY_CHECK) &&
				update_successful) {
			/* Assuming the new MFW is faulty. Reverting back to
			 * previous version. */
			schedule_modem_fw_revert();
			/* This call reboots the device before returning. */
			reboot_to_apply_update();
		}
	}

	deactivate_fota_pdn();
	restore_system_mode();

clean_exit:

	/* Erase the modem FW backup */
	erase_modem_fw_backup();

	/* Clear job ID from NV */
	save_update_job_id(NULL);
}

static void read_lte_active_time_work_fn(struct k_work *item)
{
	int err;
	char response[AT_XMONITOR_RESPONSE_MAX_LEN];
	char act_time_str[AT_XMONITOR_ACTIVE_TIME_LEN + 1];
	size_t act_time_str_len = AT_XMONITOR_ACTIVE_TIME_LEN;
	uint32_t act_time = 0;
	uint8_t act_time_unit;
	struct at_param_list param_list = {0};

	err = at_cmd_write(at_xmonitor, response, sizeof(response),
			   NULL);
	if (err) {
		LOG_ERR("Failed to read active time, error: %d", err);

		/* In this case the handler is called immediately */
		k_timer_start(&active_time_timer, K_NO_WAIT, K_NO_WAIT);
		return;
	}

	err = at_params_list_init(&param_list, AT_XMONITOR_PARAMS_COUNT_MAX);
	if (err) {
		LOG_ERR("Could not init AT params list, error: %d", err);

		/* In this case the handler is called immediately */
		k_timer_start(&active_time_timer, K_NO_WAIT, K_NO_WAIT);
		return;
	}

	err = at_parser_max_params_from_str(response, NULL, &param_list,
					    AT_XMONITOR_PARAMS_COUNT_MAX);
	if (err && err != -E2BIG) {
		LOG_ERR("Could not parse XMONITOR response, error: %d", err);
		goto clean_exit;
	}

	err = at_params_string_get(&param_list,
				   AT_XMONITOR_ACTIVE_TIME_INDEX,
				   act_time_str,
				   &act_time_str_len);
	if (err) {
		/* Active time is present only when registration status is
		 * 1 or 5. If no active time is found, the timer handler is
		 * called immediately.
		 */
		goto clean_exit;
	}

	act_time_str[act_time_str_len] = '\0';

	act_time = strtoul(act_time_str, NULL, 2);
	act_time_unit = act_time & AT_XMONITOR_ACTIVE_TIME_UNIT_MASK;
	act_time &= ~AT_XMONITOR_ACTIVE_TIME_UNIT_MASK;

	if (act_time_unit == AT_XMONITOR_ACTIVE_TIME_DISABLED) {
		LOG_DBG("PSM disabled");
		psm_enabled = false;
	} else {
		psm_enabled = true;

		/* Calculate active time */
		if (act_time_unit == AT_XMONITOR_ACTIVE_TIME_UNIT_6MIN) {
			/* Multiples of 6 minutes */
			act_time *= (6 * 60);
		} else if (act_time_unit == AT_XMONITOR_ACTIVE_TIME_UNIT_1MIN) {
			/* Multiples of 1 minute */
			act_time *= 60;
		} else {
			/* Multiples of 2 seconds */
			act_time *= 2;
		}
		LOG_DBG("PSM enabled, active time %d seconds", act_time);
	}

clean_exit:
	at_params_list_free(&param_list);

	/* Timer is started also in case the time is zero, in that case the
	 * handler is called immediately.
	 */
	k_timer_start(&active_time_timer, K_SECONDS(act_time), K_NO_WAIT);
}

static bool is_update_scheduled(void)
{
	return update_check_time_s != 0;
}

static bool is_time_for_update_check(void)
{
	return get_current_time_in_s() >= update_check_time_s;
}

static void start_update_check_timer(void)
{
	int32_t duration_s;
	uint32_t duration_s_without_days;

	if (!is_update_scheduled()) {
		LOG_ERR("Update not scheduled, can't start the timer");
	}

	duration_s = update_check_time_s - get_current_time_in_s();

	duration_s_without_days = duration_s % SECONDS_IN_DAY;
	LOG_INF("Next update check in %d days, %02d:%02d:%02d",
		duration_s / SECONDS_IN_DAY,
		duration_s_without_days / 3600,
		(duration_s_without_days % 3600) / 60,
		(duration_s_without_days % 3600) % 60);

	if (MAX_TIMER_DURATION_S < duration_s)
		duration_s = MAX_TIMER_DURATION_S;

	LOG_DBG("Starting timer for %d seconds", duration_s);

	k_timer_start(&update_check_timer, K_SECONDS(duration_s), K_NO_WAIT);
}

static void update_check_timer_handler(struct k_timer *dummy)
{
	if (is_time_for_update_check())
		start_update_check();
	else
		start_update_check_timer();
}

static void active_time_timer_handler(struct k_timer *dummy)
{
	lte_active = false;
}

static void calculate_next_update_check_time(void)
{
	uint32_t seconds_to_update_check;
	uint32_t max_rand;

	LOG_DBG("Scheduling next update check");

	seconds_to_update_check =
		(CONFIG_MODEM_FOTA_UPDATE_CHECK_INTERVAL * 60);

	max_rand = CONFIG_MODEM_FOTA_UPDATE_CHECK_INTERVAL_RANDOMNESS * 60;
	if (max_rand > 0) {
		/* Add random variation to the update check interval */
		seconds_to_update_check += sys_rand32_get() % max_rand;
	}

	update_check_time_s = get_current_time_in_s() + seconds_to_update_check;
	save_update_check_time();
}

static void calculate_postponed_update_check_time(void)
{
	uint32_t max_rand;

	LOG_DBG("Scheduling postponed update check");

	/* When update check is postponed, the update check time is moved
	 * forward by the update check interval until the time is in the
	 * future. After this the random part is added to the update check time.
	 */

	while (is_time_for_update_check()) {
		update_check_time_s +=
			CONFIG_MODEM_FOTA_UPDATE_CHECK_INTERVAL * 60;
	}

	max_rand = CONFIG_MODEM_FOTA_UPDATE_CHECK_INTERVAL_RANDOMNESS * 60;
	if (max_rand > 0) {
		/* Add random variation to the update check interval */
		update_check_time_s += sys_rand32_get() % max_rand;
	}

	save_update_check_time();
}

static bool is_update_check_timer_running(void)
{
	return k_timer_remaining_get(&update_check_timer) != 0;
}

static void schedule_next_update(void)
{
	if (!fota_enabled || !is_modem_time_valid()) {
		/* FOTA is either disabled or we haven't got time yet */
		return;
	}

	if (provisioning_enabled && !provisioning_done) {
		/* Device needs to be provisioned for FOTA service */
		k_work_submit_to_queue(&work_q, &provision_work);
		return;
	}

	if (is_update_scheduled()) {
		/* Check if the scheduled time has already passed */
		if (is_time_for_update_check()) {
			if (!is_update_check_timer_running()) {
				/* Scheduled update check time has passed while
				 * device was powered off, skip this check
				 */
				LOG_INF("Time for update check already passed, "
					"postpone check");
				calculate_postponed_update_check_time();
				start_update_check_timer();
			} else {
				/* Update check timer is running, most likely
				 * time was updated past next update check,
				 * start check now
				 */
				start_update_check();
			}
		} else {
			/* Not yet time for next update check, start
			 * timer
			 */
			start_update_check_timer();
		}
	} else {
		/* Schedule next update */
		calculate_next_update_check_time();
		start_update_check_timer();
	}
}

static void schedule_next_update_work_fn(struct k_work *item)
{
	schedule_next_update();
}

static void provision_device_work_fn(struct k_work *item)
{
	int ret;
	int retry_count;
	uint32_t start_time;
	int32_t wait_time;

	/* TODO: Wait until device is connected to network (forever?) */

	ret = read_default_apn_name();
	if (ret == 0 && default_apn != NULL) {
		if (is_apn_disabled(default_apn)) {
			LOG_INF("Default APN usage disabled, enabling APN "
				"temporarily");
			enable_apn(default_apn);
			restore_apn_status_needed = true;
		}
	}

	retry_count = CONFIG_MODEM_FOTA_SERVER_RETRY_COUNT;
	while (true) {
		LOG_INF("Provisioning device for FOTA...");

		start_time = k_uptime_get_32();

		ret = fota_client_provision_device();
		if (ret >= 0 || retry_count <= 0) {
			/* Provisioning successful or no retries left */
			break;
		}

		LOG_WRN("Provisioning failed. %d retries left...", retry_count);
		retry_count--;

		/* Make sure retries have at least 30s interval */
		wait_time = 30 * 1000 - (k_uptime_get_32() - start_time);
		if (wait_time > 0) {
			k_sleep(K_MSEC(wait_time));
		}
	}

	if (restore_apn_status_needed) {
		LOG_INF("Disabling default APN usage");
		disable_apn(default_apn);
		restore_apn_status_needed = false;
	}
	free_default_apn_name();

	if (ret == 0) {
		LOG_INF("Device provisioned, waiting 30s before using API");

		provisioning_done = true;
		save_provisioning_done();

		k_sleep(K_SECONDS(30));

/* TODO: After provisioning, the user must associate the device
 * with their nRF Cloud account. If the device is not associated,
 * it is not allowed to make the UpdateDeviceState API call.
 * For now it is recommended to have the USER make the UpdateDeviceState
 * call after they have performed association.
 * The goal is to have the device make the API call after provisioning,
 * without user interaction.
 */
#if 0
		retry_count = CONFIG_MODEM_FOTA_SERVER_RETRY_COUNT;
		LOG_INF("Setting initial device state...");
		do
		{
			ret = fota_client_set_device_state();
		} while ((ret != 0) && (retry_count--));

		if (ret != 0) {
			LOG_ERR("Failed to set deivce state, error: %d", ret);
		}
#endif
	} else if (ret == 1) {
		LOG_INF("Device already provisioned");

		provisioning_done = true;
		save_provisioning_done();
	} else {
		LOG_ERR("Error provisioning device, error: %d", ret);
	}

	if (ret == 0 || ret == 1) {
		/* Device provisioned, make sure the modem scratch area is
		 * empty when the first FOTA update is performed.
		 */
		erase_modem_fw_backup();
	}

	/* Schedule first update check (or if provisioning failed, it will be
	 * retried when the time expires.
	 */
	calculate_next_update_check_time();
	start_update_check_timer();
}

static bool prefix_matches_imsi(const char *imsi, char *prefix, int prefix_len)
{
	return strncmp(imsi, prefix, prefix_len) == 0 ? true : false;
}

static char *get_next_imsi_prefix(char **pos, int *prefix_len)
{
	char *temp_pos;
	char *prefix_start;

	temp_pos = *pos;
	prefix_start = NULL;
	*prefix_len = 0;
	while (*temp_pos != '\0') {
		if (*temp_pos == ',') {
			/* The char is a comma */
			if (prefix_start != NULL) {
				/* End of a prefix */
				break;
			}
		} else {
			/* The char is not a comma */
			if (prefix_start == NULL) {
				/* Start of a prefix */
				prefix_start = temp_pos;
			}
		}
		temp_pos++;
	}

	if (prefix_start != NULL) {
		/* Found a prefix, calculate its length */
		*prefix_len = temp_pos - prefix_start;
	}

	/* Return the current position */
	*pos = temp_pos;

	return prefix_start;
}

static bool is_fota_disabled_with_usim(void)
{
	int err;
	char imsi[15 + 2 + 1];
	char *pos;
	char *prefix;
	int prefix_len;

	if (strlen(CONFIG_MODEM_FOTA_DISABLE_IMSI_PREFIXES) == 0)
		return false;

	err = at_cmd_write(at_cimi, imsi, sizeof(imsi), NULL);
	if (err) {
		LOG_ERR("Failed to request IMSI, error: %d", err);
		return false;
	}

	pos = CONFIG_MODEM_FOTA_DISABLE_IMSI_PREFIXES;
	while ((prefix = get_next_imsi_prefix(&pos, &prefix_len)) != NULL) {
		if (prefix_matches_imsi(imsi, prefix, prefix_len)) {
			LOG_INF("IMSI matches the disable prefix list");
			return true;
		}
	}

	return false;
}

bool is_fota_enabled(void)
{
	return fota_enabled;
}

void enable_fota(void)
{
	fota_enabled = true;

	LOG_INF("FOTA enabled");

	schedule_next_update();
}

void disable_fota(void)
{
	fota_enabled = false;

	LOG_INF("FOTA disabled");

	/* Stop timer and clear the next update check time */
	k_timer_stop(&update_check_timer);
	clear_update_check_time();
}

uint32_t get_time_to_next_update_check(void)
{
	if (is_update_scheduled() && is_modem_time_valid())
		if (update_check_time_s > get_current_time_in_s()) {
			return update_check_time_s - get_current_time_in_s();
		} else {
			return 0;
		}
	else
		return 0;
}

void set_time_to_next_update_check(uint32_t seconds)
{
	update_check_time_s = get_current_time_in_s() + seconds;
	save_update_check_time();

	if (fota_enabled)
		start_update_check_timer();
}

static void parse_xtime_notification(const char *notif)
{
	bool time_already_available;

	time_already_available = is_modem_time_valid();
	if (parse_time_from_xtime_notification(notif) == 0) {
		/* Next update is scheduled/timer updated in two cases:
		 *  1) We didn't have time available yet, i.e. this is the first
		 *     XTIME notification.
		 *  2) Update check is currently not ongoing, i.e. an update
		 *     check has been scheduled and the timer is running. In
		 *     this case the time to next check is recalculated using
		 *     the updated time and timer restarted.
		 * If we already had time available but next update is not
		 * scheduled, then an update check is currently running and we
		 * don't want to schedule the next update check yet.
		 */
		if (!time_already_available || is_update_scheduled()) {
			k_work_submit(&schedule_next_update_work);
		}
	}
}

static void at_notification_handler(void *context, const char *notif)
{
	ARG_UNUSED(context);

	if (notif == NULL) {
		LOG_ERR("Notification buffer is a NULL pointer");
		return;
	}

	if (strncmp(at_cereg_notif, notif, sizeof(at_cereg_notif) - 1) == 0) {
		parse_cereg_notification(notif);
	} else if (strncmp(at_cscon_notif, notif, sizeof(at_cscon_notif) - 1)
			== 0) {
		parse_cscon_notification(notif);
	} else if (strncmp(at_xtime_notif, notif, sizeof(at_xtime_notif) - 1)
			== 0) {
		parse_xtime_notification(notif);
	}
}

static int register_xtime_notification(void)
{
	int err;

	err = at_cmd_write(at_xtime_enable, NULL, 0, NULL);
	if (err) {
		LOG_ERR("Failed to enable XTIME notification, error: %d", err);
		return err;
	}

	return err;
}

static int write_ca_cert(void)
{
	int err;
	char *buf;

	err = 0;

	buf = k_malloc(1280);
	if (buf == NULL) {
		err = -ENOMEM;
		goto clean_up;
	}

	sprintf(buf, at_cmng_write_template, CONFIG_MODEM_FOTA_TLS_SECURITY_TAG,
		AT_CMNG_TYPE_ROOT_CA_CERT, root_ca_cert);
	err = at_cmd_write(buf, NULL, 0, NULL);
	if (err) {
		goto clean_up;
	}

clean_up:
	k_free(buf);

	return err;
}

static void check_certs(void)
{
	int err;
	char buf[128];

	/* Check if the AWS root CA is stored in the modem */
	sprintf(buf, at_cmng_list_template, CONFIG_MODEM_FOTA_TLS_SECURITY_TAG,
		AT_CMNG_TYPE_ROOT_CA_CERT);
	err = at_cmd_write(buf, buf, 128, NULL);
	if (err) {
		LOG_ERR("Failed to check CA certificate, error: %d", err);
		return;
	}

	if (strlen(buf) == 0) {
		/* No CA certificate found, write it */
		err = write_ca_cert();
		if (err) {
			LOG_ERR("Failed to write CA certificate, error: %d", err);
		} else {
			LOG_INF("CA certificate written to modem");
		}
	}

	/* Check if the client certificate is stored in the modem */
	sprintf(buf, at_cmng_list_template, CONFIG_MODEM_FOTA_TLS_SECURITY_TAG,
		AT_CMNG_TYPE_CLIENT_CERT);
	err = at_cmd_write(buf, buf, 128, NULL);
	if (err) {
		LOG_ERR("Failed to check client certificate, error: %d", err);
		return;
	}

	if (strlen(buf) == 0) {
		/* No client certificate found */
		provisioning_enabled = false;
		LOG_DBG("No client certificate, provisioning disabled");
	}
}

int modem_fota_init(modem_fota_callback_t callback, const char *device_id)
{
	int err = 0;
	size_t device_id_len;

	if (callback == NULL || device_id == NULL) {
		return -EINVAL;
	}

	event_callback = callback;

	device_id_len = strlen(device_id);
	if (device_id_len > MODEM_FOTA_DEVICE_ID_MAX_LEN) {
		return -EINVAL;
	}
	strcpy(fota_device_id, device_id);

#if !defined CONFIG_MODEM_FOTA_CARRIER_LIB
	if (strlen(CONFIG_MODEM_FOTA_APN) > 0) {
		fota_apn = CONFIG_MODEM_FOTA_APN;
	} else {
		fota_apn = NULL;
	}
#else
	fota_apn = modem_fota_carrier_get_apn();
#endif

	k_sem_init(&attach_sem, 0, 1);
	k_sem_init(&detach_sem, 0, 1);
	k_sem_init(&rrc_idle_sem, 0, 1);
	k_sem_init(&lte_active_sem, 0, 1);

	k_work_q_start(&work_q, work_q_stack_area,
        	K_THREAD_STACK_SIZEOF(work_q_stack_area), WORK_QUEUE_PRIORITY);
	k_work_init(&provision_work, provision_device_work_fn);
	k_work_init(&update_work, start_update_work_fn);
	k_work_init(&current_update_info.finish_update_work, finish_update_work_fn);
	k_work_init(&update_job_status_work, update_job_status_work_fn);
	k_work_init(&read_lte_active_time_work, read_lte_active_time_work_fn);
	k_work_init(&schedule_next_update_work, schedule_next_update_work_fn);
	k_delayed_work_init(&retry_download_work, retry_download_work_fn);

	init_and_load_settings();

	err = fota_download_init(&fota_download_callback);
	if (err) {
		LOG_ERR("FOTA download library could not be initialized, "
			"error: %d", err);
		return err;
	}

	at_notif_register_handler(NULL, at_notification_handler);
	register_xtime_notification();

	check_certs();

	if (update_job_id != NULL) {
		update_job_status_after_apply();
	}

	LOG_INF("FOTA Client initialized");

	return err;
}

void modem_fota_configure(void)
{
	/* We can read the IMSI and check if FOTA needs to be
	 * disabled with this USIM.
	 */
	if (is_fota_disabled_with_usim()) {
		disable_fota();
	}
}

int modem_fota_set_clock(const char *time_str)
{
	int err;
	char cclk_command[40];

	if (time_str == NULL || strlen(time_str) < AT_CCLK_TIME_LEN) {
		return -EINVAL;
	}

	/* Set time to modem */
	strcpy(cclk_command, "AT+CCLK=\"");
	strcat(cclk_command, time_str);
	strcat(cclk_command, "\"");
	err = at_cmd_write(cclk_command, NULL, 0, NULL);
	if (err) {
		LOG_ERR("CCLK failed, error: %d", err);
		return err;
	}

	parse_cclk_time(time_str);

	k_work_submit(&schedule_next_update_work);

	return err;
}
