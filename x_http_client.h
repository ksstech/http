/*
 * x_http_client.h
 */

#pragma once

#include 	"x_http_common.h"

#include	<stdint.h>
#include	<stdbool.h>

// ###################################### BUILD : CONFIG definitions ###############################


// ########################################### macros #############################################


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

int32_t xHttpClientCheckUpgrades(bool bCheck) ;

int32_t xHttpGetLocation(void) ;
int32_t	xHttpGetElevation(void) ;
int32_t xHttpGetTimeZone(void) ;
int32_t	xHttpClientCheckGeoLoc(void) ;

int32_t	xHttpClientPushOver(const char * pcMess, uint32_t) ;

int32_t	xHttpClientRulesDownload(void) ;

int32_t	xHttpClientIdentUpload(void * pvPara) ;
int32_t	xHttpClientCoredumpUpload(void * pvPara) ;

#ifdef __cplusplus
}
#endif
