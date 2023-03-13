/*
 * x_http_client.h
 */

#pragma once

#include 	"x_http_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// ###################################### BUILD : CONFIG definitions ###############################


// ########################################### macros #############################################


// ######################################### enumerations ##########################################

enum { PERFORM = 0, CHECK = 1 } ;

// ######################################### structures ############################################


// ################################### Global variables ############################################


// ###################################### public functions #########################################

int	xHttpCompleteRequest(http_rr_t * psReq) ;
int	xHttpParseResponse(char * pBuf, size_t xLen) ;
int	xHttpRequest(pcc_t pHost, pcc_t pQuery, const void * pvBody,
		pcc_t pcCert, size_t szCert, void * OnBodyCB, u32_t DataSize,
		u32_t hvValues, u16_t BufSize, netx_dbg_t Debug, void * pvArg, ...);

int xHttpGetWeather(void) ;
int	xHttpHowsMySSL(void) ;
int	xHttpBadSSL(void) ;

int xHttpClientCheckUpgrades(bool bCheck) ;

int xHttpGetLocation(void) ;
int	xHttpGetElevation(void) ;
int xHttpGetTimeZone(void) ;

int	xHttpClientPushOver(const char * pcMess, u32_t) ;

int	xHttpClientRulesDownload(void) ;

int	xHttpClientIdentUpload(void * pvPara) ;
int	xHttpCoredumpUpload(void);

#ifdef __cplusplus
}
#endif
