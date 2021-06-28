/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @file location.h
 * @brief Public APIs for the Location library.
 * @defgroup location Location library
 * @{
 */

#ifndef LOCATION_H_
#define LOCATION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOC_MAX_METHODS 2

/** Positioning methods. */
enum loc_method {
	/** LTE cellular positioning. */
	LOC_METHOD_CELL_ID = 1,
	/** Global Navigation Satellite System (GNSS). */
	LOC_METHOD_GNSS,
};

/** Event IDs. */
enum loc_event_id {
	/** Location update. */
	LOC_EVT_LOCATION,
	/** Getting location timed out. */
	LOC_EVT_TIMEOUT,
	/** An error occurred when trying to get the location. */
	LOC_EVT_ERROR
};

/** Positioning accuracy enumerator. */
enum loc_accuracy {
	/** Allow lower accuracy to conserve power. */
	LOC_ACCURACY_LOW,
	/** Normal accuracy. */
	LOC_ACCURACY_NORMAL,
	/** Allow higher power consumption to improve accuracy. */
	LOC_ACCURACY_HIGH
};

/** Date and time (UTC). */
struct loc_datetime {
	/** True if date and time are valid, false if not. */
	bool valid;
	/** 4-digit representation (Gregorian calendar). */
	uint16_t year;
	/** 1...12 */
	uint8_t month;
	/** 1...31 */
	uint8_t day;
	/** 0...23 */
	uint8_t hour;
	/** 0...59 */
	uint8_t minute;
	/** 0...59 */
	uint8_t second;
	/** 0...999 */
	uint16_t ms;
};

/** Location data. */
struct loc_location {
	/** Geodetic latitude (deg) in WGS-84. */
	double latitude;
	/** Geodetic longitude (deg) in WGS-84. */
	double longitude;
	/** Location accuracy in meters (1-sigma). */
	float accuracy;
	/** Date and time (UTC). */
	struct loc_datetime datetime;
};

/** Error cause. */
struct loc_cause {
	/* TODO: Error codes? */
	uint8_t error;
};

/** Location event data. */
struct loc_event_data {
	/** Event ID. */
	enum loc_event_id id;
	/** Used positioning method. */
	enum loc_method method;
	union {
		/** Current location, used with event LOC_EVT_LOCATION. */
		struct loc_location location;
		/** Error cause, used with event LOC_EVT_ERROR. */
		struct loc_cause cause;
	};
};

/** LTE cellular positioning configuration. */
struct loc_cell_id_config {
};

/** GNSS configuration. */
struct loc_gnss_config {
	/** Timeout, i.e. how long GNSS is allowed to run when trying to acquire a fix. */
	uint16_t timeout;
	/** Desired accuracy level. */
	enum loc_accuracy accuracy;
};

/** Positioning method configuration. */
struct loc_method_config {
	/** Positioning method. */
	enum loc_method method;
	union {
		/** Configuration for LOC_METHOD_CELL_ID. */
		struct loc_cell_id_config cell_id;
		/** Configuration for LOC_METHOD_GNSS. */
		struct loc_gnss_config gnss;
	} config;
};

/** Location request configuration. */
struct loc_config {
	/** Selected positioning methods and associated configurations in priority order. Index 0
	 *  has the highest priority. Unused entries in the array must be initialized to zero.
	 */
	struct loc_method_config methods[LOC_MAX_METHODS];
	/** Position update interval in seconds. Set to 0 for a single position update. For periodic
	 *  position updates the valid range is 10...65535 seconds.
	 */
	uint16_t interval;
};

/** @brief Event handler prototype.
 *
 * @param[in] event_data Pointer to event data.
 */
typedef void (*location_event_handler_t)(const struct loc_event_data *event_data);

/** @brief Initializes the library.
 *
 * @details Initializes the library and sets the event handler function.
 *
 * @param[in] event_handler Event handler function.
 *
 * @return 0 on success, or negative error code on failure.
 */
int location_init(location_event_handler_t event_handler);

/** @brief Requests the current position or starts periodic position updates.
 *
 * @details Requests the current position using the given configuration. Depending on the
 *          configuration, a single position or periodic position updates are given. The results are
 *          delivered to the event handler function.
 *
 *          Periodic position updates can be stopped by calling location_cancel().
 *
 * @param[in] config Pointer to the used configuration or NULL to get a single position update with
 *                   the default configuration.
 *
 * @return 0 on success, or negative error code on failure.
 */
int location_request(const struct loc_config *config);

/** @brief Cancels periodic position updates.
 *
 * @return 0 on success, or negative error code on failure.
 */
int location_request_cancel(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* LOCATION_H_ */
