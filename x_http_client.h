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
		u32_t u32 ;
		struct {
			bool	http;
			bool	open;
			bool	write;
			bool	read;
			bool	data;
			bool	eagain;
			bool	secure;
			bool	verify;
			u8_t	level:2;
		};
	};
} xnet_debug_t ;

// ################################### Global variables ############################################


// ###################################### public functions #########################################

int	xHttpCompleteRequest(http_rr_t * psReq) ;
int	xHttpParseResponse(char * pBuf, size_t xLen) ;
int	xHttpClientExecuteRequest(http_rr_t * psReq, va_list vArgs) ;
int	xHttpRequest(pci8_t pHost, pci8_t pQuery, const void * pvBody,
		pci8_t pcCert, size_t szCert, void * OnBodyCB, u32_t DataSize,
		u32_t hvValues, u16_t BufSize, xnet_debug_t Debug, void * pvArg, ...) ;

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
