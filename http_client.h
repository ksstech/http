/*
 * http_client.h
 */

#pragma once

#include 	"http_common.h"

#include	"x_config.h"								// required for API keys...
#include	"x_sockets.h"
#include	"x_definitions.h"
#include	"x_time.h"

#include	"stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

// ###################################### BUILD : CONFIG definitions ###############################


// ########################################### macros #############################################

/*
 * Open Weather Map service definitions
 */
#ifndef		configHTTP_APIKEY_OPEN_WEATHERMAP
	#error	" Open WeatherMap : API key undefined, normally in x_config.h in application directory"
#endif

#define	configHTTP_HOST_OPEN_WEATHERMAP			"api.openweathermap.org"
#define	configHTTP_FCAST_OPEN_WEATHERMAP		"GET /data/2.5/forecast/?q=Johannesburg,ZA"
#define	configHTTP_FCAST_JOHANNESBURG			configHTTP_FCAST_OPEN_WEATHERMAP"&APPID="configHTTP_APIKEY_OPEN_WEATHERMAP

// https://www.iplocation.net/

/*
 * EurekAPI.com (basic free 30day, 2017/06/06)
 */
#ifndef		configHTTP_APIKEY_EUREKAPI
	#error	" EurekAPI.com: API key undefined, normally in x_config.h in application directory"
#endif

#define	configHTTP_HOST_EUREKAPI				"api.eurekapi.com"
#define	configHTTP_QUERY_EUREKAPI				"GET /iplocation/v1.8/locateip?key="configHTTP_APIKEY_EUREKAPI"&ip=196.201.119.114&format=JSON&compact=Y"

/*
 * free geo-ip service definition
 */
#define	configHTTP_HOST_FREE_GEO_IP				"freegeoip.net"
#define	configHTTP_FORMAT_FREE_GEO_IP			"GET /json"

/*
 * Google geolocation API
 *		https://developers.google.com/maps/documentation/geolocation/intro
 * Example:
 *		https://www.googleapis.com/geolocation/v1/geolocate?key=YOUR_API_KEY
 */
#define	googleAPI_GEOLOC_HOST					"www.googleapis.com"
#define	googleAPI_GEOLOC_QUERY					"POST /geolocation/v1/geolocate?key="configHTTP_APIKEY_GOOGLE_PAIDKEY
#define	googleAPI_GEOLOC_BODY					"{ }\r\n"

/*
 * Google elevation API
 *		https://developers.google.com/maps/documentation/elevation/start
 * Example:
 *		https://maps.googleapis.com/maps/api/elevation/json?locations=39.7391536,-104.9847034&key=YOUR_API_KEY
 */
//#define	googleAPI_ELEVATION_HOST				"maps.googleapis.com"
#define	googleAPI_ELEVATION_QUERY				"GET /maps/api/elevation/json?locations=%.7f,%.7f&key="configHTTP_APIKEY_GOOGLE_PAIDKEY

/*
 * Google TimeZone API
 *		https://developers.google.com/maps/documentation/timezone/intro#Requests
 * Example:
 * 		https://maps.googleapis.com/maps/api/timezone/json?location=38.908133,-77.047119&timestamp=1458000000&key=YOUR_API_KEY
 */
//#define	googleAPI_TIMEZONE_HOST					"maps.googleapis.com"
#define	googleAPI_TIMEZONE_QUERY				"GET /maps/api/timezone/json?location=%.7f,%.7f&timestamp=%d&key="configHTTP_APIKEY_GOOGLE_PAIDKEY

// ######################################### enumerations ##########################################


// ######################################### structures ############################################

// ################################### Global variables ############################################


// ###################################### public functions #########################################

int32_t	xHttpCompleteRequest(http_reqres_t * psReq) ;
int32_t	xHttpParseResponse(char * pBuf, size_t xLen) ;
int32_t	xHttpClientExecuteRequest(http_reqres_t * psReq, ...) ;
int32_t	xHttpClientFileDownloadCheck(http_parser * psParser, const char * pBuf, size_t xLen) ;

int32_t xHttpGetWeather(void) ;
int32_t	xHttpHowsMySSL(void) ;
int32_t	xHttpBadSSL(void) ;

int32_t	xHttpClientFirmwareUpgrade(void * pvPara) ;
int32_t xHttpClientCheckUpgrades(void) ;

int32_t xHttpGetLocation(void) ;
int32_t	xHttpGetElevation(void) ;
int32_t xHttpGetTimeZone(void) ;

int32_t	xHttpClientRulesDownload(void) ;

int32_t	xHttpClientIdentUpload(void * pvPara) ;
int32_t	xHttpClientCoredumpUpload(void * pvPara) ;

#ifdef __cplusplus
}
#endif
