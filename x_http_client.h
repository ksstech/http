/*
 * x_http_client.h
 */

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

// ###################################### BUILD : CONFIG definitions ###############################


// ########################################### macros #############################################

#if (includeHTTP_TASK > 0)
	#define flagTASK_TO_TEST flagHTTP_SERV
#elif (includeTNET_TASK > 0)
	#define flagTASK_TO_TEST flagTNET_SERV
#else
	#error "No task defined to handle HTTP requests"
#endif

// ######################################### enumerations ##########################################

enum { PERFORM = 0, CHECK = 1 };

enum {								// HTTP requests
	reqGEOALT		= (1 << 0),
	reqGEOTZ		= (1 << 1),
	reqGEOLOC		= (1 << 2),
	reqFW_CHECK		= (1 << 3),
	reqFW_UPGRADE	= (1 << 4),
	reqCOREDUMP		= (1 << 5),
	// add extra here
	reqNOTIFIED		= (1 << 24)		// flag used to indicate "have sent request"
};


// ######################################### structures ############################################


// ################################### Global variables ############################################


// ###################################### public functions #########################################

void vHttpRequestNotifyTask(u32_t ulValue);
int vHttpRequestNotifyHandler(void);
int	xHttpRequest(pcc_t pHost, pcc_t pQuery, const void * pvBody,
		pcc_t pcCert, size_t szCert, void * OnBodyCB, u32_t DataSize,
		u32_t hvValues, u16_t BufSize, netx_dbg_t Debug, void * pvArg, ...);
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

#ifdef __cplusplus
}
#endif
