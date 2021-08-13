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

#define	xnetDEBUG_FLAGS(A, B, C, D, E, F, G, H, I) (xnet_debug_t) \
	{	.http=A, .open=B, .write=C, .read=D, .data=E,	\
		.eagain=F,	.secure=G, .verify=H, .level=I	\
	}

// ######################################### enumerations ##########################################

enum { PERFORM = 0, CHECK = 1 } ;

// ######################################### structures ############################################

typedef	struct xnet_debug_t {
	union {
		uint32_t u32 ;
		struct {
			bool	http ;
			bool	open ;
			bool	write ;
			bool	read ;
			bool	data ;
			bool	eagain ;
			bool	secure ;
			bool	verify ;
			uint8_t	level	: 3 ;
		} ;
	} ;
} xnet_debug_t ;

// ################################### Global variables ############################################


// ###################################### public functions #########################################

int32_t	xHttpCompleteRequest(http_rr_t * psReq) ;
int32_t	xHttpParseResponse(char * pBuf, size_t xLen) ;
int32_t	xHttpClientExecuteRequest(http_rr_t * psReq, va_list vArgs) ;
int32_t	xHttpRequest(pci8_t pHost, pci8_t pQuery, const void * pvBody,
		pci8_t pcCert, size_t szCert, void * OnBodyCB, uint32_t DataSize,
		uint32_t hvValues, uint16_t BufSize, xnet_debug_t Debug, void * pvArg, ...) ;

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
