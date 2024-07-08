// x_http_client.h

#pragma once

#if (includeHTTP_TASK > 0)
	#include "x_http_server.h"
#elif (includeTNET_TASK > 0)
	#include "tnet_server.h"
#endif
#include "x_http_common.h"								// for xHttpFirmware????()

#ifdef __cplusplus
extern "C" {
#endif

// ########################################### Macros ##############################################
// ######################################## Enumerations ###########################################

enum { PERFORM = 0, CHECK = 1 };
enum { httpFW_ERROR = -1, httpFW_OLD_FOUND = 0, httpFW_NEW_FOUND = 1 };

enum {													// REQuest NUMbers
	reqNUM_COREDUMP,
	reqNUM_FW_UPG1,
	reqNUM_FW_UPG2,
	reqNUM_FW_CHK1,
	reqNUM_FW_CHK2,
	reqNUM_GEOLOC,
	reqNUM_GEOTZ,
	reqNUM_GEOALT,
	reqNUM_NOTIFIED = 24,
};

enum {													// REQuest MASKs
	reqCOREDUMP		= (1 << reqNUM_COREDUMP),			// highest priority
	reqFW_UPG1		= (1 << reqNUM_FW_UPG1),
	reqFW_UPG2		= (1 << reqNUM_FW_UPG2),
	reqFW_UPGRADE	= (reqFW_UPG1 | reqFW_UPG2),
	reqFW_CHK1		= (1 << reqNUM_FW_CHK1),
	reqFW_CHK2		= (1 << reqNUM_FW_CHK2),
	reqFW_CHECK		= (reqFW_CHK1 | reqFW_CHK2),
	reqGEOLOC		= (1 << reqNUM_GEOLOC),
	reqGEOTZ		= (1 << reqNUM_GEOTZ),
	reqGEOALT		= (1 << reqNUM_GEOALT),
	reqNOTIFIED		= (1 << reqNUM_NOTIFIED)			// flag used to indicate "have sent request"
};

// ######################################### Structures ############################################
// ###################################### Global variables #########################################
// ###################################### Public functions #########################################

bool bHttpRequestNotifyTask(u32_t ulValue);
int vHttpRequestNotifyHandler(void);
int xHttpGetWeather(void);
int	xHttpHowsMySSL(int ioHost);
int	xHttpBadSSL(int ioHost);
int xHttpClientCheckUpgrades(bool bCheck);
int xHttpGetLocation(void);
int	xHttpGetElevation(void);
int xHttpGetTimeZone(void);
int	xHttpClientPushOver(const char * pcMess, u32_t);
int	xHttpClientRulesDownload(void);
int	xHttpClientIdentUpload(void * pvPara);
int	xHttpCoredumpUpload(void);

TaskHandle_t xHttpClientTaskStart(void * pvPara);

#ifdef __cplusplus
}
#endif
