/*
 * Copyright 2014-18 Andre M Maree / KSS Technologies (Pty) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/*
 * http_client.c
 */

#include 	"http_client.h"

#include	"sitewhere.h"
#include	"sitewhere-text.h"							// for vMqttSubscribeRulesTEXT()
#include	"FreeRTOS_Support.h"
#include	"actuators.h"

#include	"x_debug.h"
#include	"x_json_parser.h"							// parsing location & TZ requests
#include	"x_errors_events.h"
#include	"x_syslog.h"
#include	"x_systiming.h"

#include	"hal_network.h"
#include	"hal_fota.h"								// firmware download handler
#include	"hal_mcu.h"									// for halMCU_Restart()

#include	<string.h>

// ############################### BUILD: debug configuration options ##############################

#define	debugFLAG						0x4008
#define	debugJSON						(debugFLAG & 0x0001)
#define	debugTRACK						(debugFLAG & 0x0002)
#define	debugBUILD						(debugFLAG & 0x0004)
#define	debugTIMING						(debugFLAG & 0x0008)

#define	debugPARAM						(debugFLAG & 0x4000)
#define	debugRESULT						(debugFLAG & 0x8000)

// ###################################### BUILD : CONFIG definitions ###############################

#define	configHTTP_CLIENT_TLS			1
#define configHTTP_BUFSIZE				4096

// ################################### global/public variables #####################################


// ################################### Common HTTP API functions ###################################

int32_t	xHttpBuildRequest(http_parser * psParser) {
	http_reqres_t * psRR = psParser->data ;
	IF_myASSERT(debugPARAM, INRANGE_SRAM(psParser) && INRANGE_SRAM(psRR) && INRANGE_SRAM(psRR->sBuf.pBuf)) ;

	vuprintf(&psRR->sBuf, psRR->pcQuery, psRR->VaList) ;
	uprintf(&psRR->sBuf, " HTTP/1.1\r\n") ;
	uprintf(&psRR->sBuf, "Host: %s\r\n", psRR->sCtx.pHost) ;
	uprintf(&psRR->sBuf, "From: %s\r\n", httpFROM_NAME) ;
	uprintf(&psRR->sBuf, "User-Agent: %s\r\n", httpAGENT_NAME) ;
	if (psRR->hvAccept) {
		uprintf(&psRR->sBuf, "Accept: %s\r\n", ctValues[psRR->hvAccept]) ;
		psRR->hvAccept	= ctUNDEFINED ;
	}
	if (psRR->hvConnect) {
		uprintf(&psRR->sBuf, "Connection: %s\r\n", coValues[psRR->hvConnect]) ;
	}
	// from here on items common to requests and responses...
	if (psRR->pcBody) {
		if (psRR->hvContentType) {
			uprintf(&psRR->sBuf, "Content-Type: %s\r\n", ctValues[psRR->hvContentType]) ;
			if (psRR->hvContentType == ctApplicationOctetStream) {
			// assume pcBody is pointing to actual binary payload
				myASSERT(INRANGE_MEM(psRR->pcBody) && INRANGE(1, psRR->hvContentLength, MEGA, uint64_t)) ;
				uprintf(&psRR->sBuf, "Content-Length: %d\r\n", psRR->hvContentLength) ;
				// no actual binary content added, done later...
			} else {
			// currently handle json/xml/text/html here, determine length using NULL string buffer address
				psRR->hvContentLength = xvsprintf(NULL, psRR->pcBody, psRR->VaList) ;
			// then add the calculated content length
				uprintf(&psRR->sBuf, "Content-Length: %d\r\n\r\n", psRR->hvContentLength) ;
			// add the actual content after 2x CR/LF pairs
				vuprintf(&psRR->sBuf, psRR->pcBody, psRR->VaList) ;
			}
		} else {
			myASSERT(0) ;								// must have body AND content type
		}
	}
	// add the final CR after the headers (and payload)
	uprintf(&psRR->sBuf, "\r\n") ;
#if		(myDEBUG == 1)
	if (psRR->f_debug) {
		PRINT("Content:\n%.*s", psRR->sBuf.Used, psRR->sBuf.pBuf) ;
	}
#endif
	return psRR->sBuf.Used ;
}

int32_t	xHttpClientExecuteRequest(http_reqres_t * psRR, ...) {
	IF_myASSERT(debugPARAM, INRANGE_SRAM(psRR)) ;
	IF_SYSTIMER_START(debugTIMING, systimerHTTP) ;
	http_parser sParser ;
	http_parser_init(&sParser, HTTP_RESPONSE) ;			// clear all parser fields/values
	sParser.data	= psRR ;

	// setup the uubuf_t structure for printing
	xUBufCreate(&psRR->sBuf, NULL, (psRR->sBuf.Size == 0) ? configHTTP_BUFSIZE	: psRR->sBuf.Size, 0) ;

	va_start(psRR->VaList, psRR) ;
	int32_t xLen = xHttpBuildRequest(&sParser) ;		// build the request
	va_end(psRR->VaList) ;

// open a socket connection to the host
	psRR->sCtx.type				= SOCK_STREAM ;
	psRR->sCtx.sa_in.sin_family	= AF_INET ;
	if (psRR->sCtx.sa_in.sin_port	== 0) {
			psRR->sCtx.sa_in.sin_port	= htons(psRR->sCtx.psSec ? IP_PORT_HTTPS : IP_PORT_HTTP) ;
	}
	int32_t iRetVal = xNetOpen(&psRR->sCtx) ;
	if (iRetVal == erSUCCESS) {							// if socket=open, write request
		iRetVal = xNetWrite(&psRR->sCtx, psRR->sBuf.pBuf, xLen) ;
		if (iRetVal > 0) {								// successfully written some (or all)
			if (psRR->hvContentType == ctApplicationOctetStream) {
				iRetVal = psRR->handler(psRR) ;			// should return same as xNetWrite()
			}
			if (iRetVal > 0) {							// now do the actual read
				iRetVal = xNetReadBlocks(&psRR->sCtx, psRR->sBuf.pBuf, psRR->sBuf.Size, configHTTP_RX_WAIT) ;
				if (iRetVal > 0) {						// actually read something
					psRR->sBuf.Used = iRetVal ;
					iRetVal = xHttpCommonDoParsing(&sParser) ;
				} else {
					IF_SL_DBG(debugTRACK, " nothing read ie to parse") ;
				}
			} else {
				IF_SL_DBG(debugTRACK, " nothing written (by handler) so can't expect to read") ;
			}
		} else {
			IF_SL_DBG(debugTRACK, " no header written, so can't expect to read") ;
		}
	} else {
		IF_SL_DBG(debugTRACK, " could not open connection") ;
	}
	xNetClose(&psRR->sCtx) ;							// close the socket connection if still open...
	vUBufDestroy(&psRR->sBuf) ;							// return memory allocated
	IF_SYSTIMER_STOP(debugTIMING, systimerHTTP) ;
	return iRetVal ;
}

int32_t	xHttpClientFileDownloadCheck(http_parser * psParser, const char * pBuf, size_t xLen) {
	if (psParser->status_code != HTTP_STATUS_OK) {
		IF_SL_DBG(debugTRACK, "file not found") ;
		return erFAILURE ;
	}
	http_reqres_t * psReq = psParser->data ;
	if ((psReq->hvContentLength == 0ULL) || (xLen == 0UL)) {
		SL_ERR("Invalid size: file (%llu) or block (%u)", psReq->hvContentLength, xLen) ;
		return erFAILURE ;
	}
	if (psReq->hvContentType != ctApplicationOctetStream) {
		SL_ERR("Invalid content type=%d %*s", psReq->hvContentType, xLen, pBuf) ;
		return erFAILURE ;
	}
	if (psReq->hvConnect == coClose) {
		SL_ERR("Connection closed %.*s", xLen, pBuf) ;
		return erFAILURE ;
	}

	return erSUCCESS ;
}

// ###################################### WEATHER support ##########################################

int32_t xHttpGetWeather(void) {
	http_reqres_t sReq	= { 0 } ;
	sReq.pcQuery		= configHTTP_FCAST_JOHANNESBURG ;
	sReq.sCtx.pHost		= configHTTP_HOST_OPEN_WEATHERMAP ;
	sReq.sBuf.Size		= 16384 ;
	return xHttpClientExecuteRequest(&sReq) ;
}

// ################################### How's my SSL support ########################################

int32_t	xHttpHowsMySSL(void) {
	sock_sec_t sSecure	= { 0 } ;
	sSecure.pPem		= HostInfo[nvsVars.HostFOTA].pCert ;
	sSecure.PemSize		= HostInfo[nvsVars.HostFOTA].Size ;
	http_reqres_t sReq	= { 0 } ;
	sReq.sCtx.psSec		= &sSecure ;
	sReq.sCtx.pHost		= "www.howsmyssl.com" ;
	sReq.pcQuery		= "GET /a/check" ;
	sReq.hvContentType	= ctUNDEFINED ;
	sReq.hvAccept		= ctUNDEFINED ;
	sReq.f_debug		= 1 ;
	return xHttpClientExecuteRequest(&sReq) ;
}

// ####################################### Bad SSL support #########################################

int32_t	xHttpBadSSL(void) {
	sock_sec_t sSecure	= { 0 } ;
	sSecure.pPem		= HostInfo[nvsVars.HostFOTA].pCert ;
	sSecure.PemSize		= HostInfo[nvsVars.HostFOTA].Size ;
	http_reqres_t sReq	= { 0 } ;
	sReq.sCtx.psSec		= &sSecure ;
	sReq.sCtx.pHost		= "www.badssl.com" ;
	sReq.pcQuery		= "GET /dashboard" ;
	sReq.f_debug		= 1 ;
	return xHttpClientExecuteRequest(&sReq) ;
}

// ################################# Firmware Over The Air support #################################

int32_t	xHttpClientFirmwareUpgrade(void * pvPara) {
	sock_sec_t sSecure	= { 0 } ;
	sSecure.pPem		= HostInfo[nvsVars.HostFOTA].pCert ;
	sSecure.PemSize		= HostInfo[nvsVars.HostFOTA].Size ;
	http_reqres_t sReq	= { 0 } ;
	sReq.sCtx.psSec		= &sSecure ;
	sReq.sCtx.pHost		= HostInfo[nvsVars.HostFOTA].pName ;
	sReq.pcQuery		= "GET /firmware/%s.bin" ;
	sReq.hvAccept		= ctApplicationOctetStream ;
	sReq.hvConnect		= coKeepAlive ;
#if 0
	sReq.f_debug		= 1 ;
	sReq.sCtx.d_open	= 1 ;
	sReq.sCtx.d_secure	= 1 ;
	sReq.sCtx.d_write	= 1 ;
	sReq.sCtx.d_read	= 1 ;
//	sReq.sCtx.d_data	= 1 ;
//	sReq.sCtx.d_eagain	= 1 ;
#endif
	sReq.sfCB.on_body	= halFOTA_HttpOnBody ;
	int32_t iRetVal		= xHttpClientExecuteRequest(&sReq, pvPara) ;
	// iRetVal here should be the number of bytes parsed or erFAILURE (-1)
	if (iRetVal > erSUCCESS) {
		if (sReq.hvLastModified < (BuildSeconds + fotaMIN_DIF_SECONDS)) {
			return erSUCCESS ;
		} else {
			SL_WARN("Restart '%s'", pvPara) ;
			vRtosSetStatus(flagAPP_RESTART) ;
			vRtosClearStatus(flagAPP_UPGRADE) ;
		}
	} else if (sReq.hvStatus == HTTP_STATUS_NOT_FOUND) {
		return erSUCCESS ;
	}
	return erFAILURE ;
}

int32_t xHttpClientCheckUpgrades(void) {
	/* To create a hierarchy of firmware upgrades, we need to define a descending order:
	 * #1 and most specific would be MAC specific "1234567890ab.bin"
	 * #2 would be "[site-token].bin"
	 * #3 would define a level to accommodate a specific client/tenant
	 * #4 would be the broadest "[device-specification-token].bin" */
	int32_t iRetVal = xHttpClientFirmwareUpgrade((void *) idSTA) ;
	#if 0
	if (xRtosCheckStatus(flagAPP_UPGRADE)) {
		iRetVal = xHttpClientFirmwareUpgrade((void *) mqttSITE_TOKEN) ;
	}
	#endif
	if (xRtosCheckStatus(flagAPP_UPGRADE)) {
		iRetVal = xHttpClientFirmwareUpgrade((void *) mqttSPECIFICATION_TOKEN) ;
	}
	if (iRetVal == erSUCCESS) { 					// no [newer] upgrade there
		vRtosClearStatus(flagAPP_UPGRADE) ;			// then clear the flag
	}
	return iRetVal ;
}

// ########################################## Location #############################################

int32_t	xHttpParseGeoLoc(http_parser* psParser, const char* pBuf, size_t xLen) {
	int32_t		iRetVal = erFAILURE, NumTok ;
	const char * pKey = " Insufficient" ;
	jsmn_parser	sParser ;
	jsmntok_t *	psTokenList ;
	NumTok = xJsonParse((uint8_t *) pBuf, xLen, &sParser, &psTokenList) ;
	if (NumTok > 0) {								// parse Latitude
		iRetVal = xJsonParseKeyValue(pBuf, psTokenList, NumTok, pKey = "lat", &nvsVars.GeoLocation[Latitude], vfFXX) ;
		if (iRetVal >= erSUCCESS) {					// parse Longitude
			iRetVal = xJsonParseKeyValue(pBuf, psTokenList, NumTok, pKey = "lng", &nvsVars.GeoLocation[Longitude], vfFXX) ;
			if (iRetVal >= erSUCCESS) {				// parse accuracy
				iRetVal = xJsonParseKeyValue(pBuf, psTokenList, NumTok, pKey = "accuracy", &nvsVars.GeoLocation[Accuracy], vfFXX) ;
			}
		}
	}
	if (iRetVal < erSUCCESS) {
		SL_ERR("Parsing '%s' key", pKey) ;
	} else {
		if (nvsVars.GeoLocation[Latitude] &&
			nvsVars.GeoLocation[Longitude]) {
			nvsVars.fGeoLoc = 1 ;
			VarsFlag |= varFLAG_LOCATION ;
		}
		IF_PRINT(debugRESULT, "lat: %.7f lng: %.7f acc: %.7f\n",
				nvsVars.GeoLocation[Latitude], nvsVars.GeoLocation[Longitude], nvsVars.GeoLocation[Accuracy]) ;
	}
	IF_EXEC_4(debugJSON, xJsonPrintTokens, (uint8_t *) pBuf, psTokenList, NumTok, 0) ;
	if (psTokenList) {
		vPortFree(psTokenList) ;
	}
    return iRetVal ;
}

int32_t	xHttpGetLocation(void) {
	if (nvsVars.fGeoLoc &&
		nvsVars.GeoLocation[Latitude] &&
		nvsVars.GeoLocation[Longitude]) {
		return erSUCCESS ;
	}
	sock_sec_t sSecure	= { 0 } ;
	sSecure.pPem		= HostInfo[hostGOOGLE].pCert ;
	sSecure.PemSize		= HostInfo[hostGOOGLE].Size ;
	http_reqres_t sReq	= { 0 } ;
	sReq.sCtx.psSec		= &sSecure ;
	sReq.sCtx.pHost		= googleAPI_GEOLOC_HOST ;		// not standard maps URL
	sReq.pcQuery		= googleAPI_GEOLOC_QUERY ;
	sReq.pcBody			= googleAPI_GEOLOC_BODY ;
	sReq.hvContentType	= ctApplicationJson ;
	sReq.hvAccept		= ctApplicationJson ;
	sReq.sfCB.on_body	= xHttpParseGeoLoc  ;
#if 0
	sReq.f_debug		= 1 ;
	sReq.sCtx.d_open	= 1 ;
	sReq.sCtx.d_secure	= 1 ;
	sReq.sCtx.d_write	= 1 ;
	sReq.sCtx.d_read	= 1 ;
	sReq.sCtx.d_data	= 1 ;
	sReq.sCtx.d_eagain	= 1 ;
#endif
	return xHttpClientExecuteRequest(&sReq) ;
}

// ##################################### TIMEZONE support ##########################################

int32_t	xHttpParseTimeZone(http_parser* psParser, const char* pBuf, size_t xLen) {
	jsmn_parser	sParser ;
	jsmntok_t *	psTokenList ;
	int32_t		NumTok, iRetVal = erFAILURE ;
	const char * pKey = " Insufficient" ;
	NumTok = xJsonParse((uint8_t *) pBuf, xLen, &sParser, &psTokenList) ;
	if (NumTok > 0) {
		x32_t	xVal ;
		iRetVal = xJsonParseKeyValue(pBuf, psTokenList, NumTok, pKey = "dstOffset", &xVal.i32, vfIXX) ;
		if (iRetVal >= erSUCCESS) {
			sTZ.daylight = xVal.i32 ;					// convert i32 -> i16 & store
			iRetVal = xJsonParseKeyValue(pBuf, psTokenList, NumTok, pKey = "rawOffset", &xVal.i32, vfIXX) ;
			if (iRetVal >= erSUCCESS) {
				sTZ.timezone = xVal.i32 ;				// store value
				iRetVal = xJsonParseKeyValue(pBuf, psTokenList, NumTok, pKey = "timeZoneId", nvsVars.TimeZoneId, vfSXX) ;
				if (iRetVal >= erSUCCESS) {
					sTZ.pcTZName = nvsVars.TimeZoneId ;
					iRetVal = xJsonParseKeyValue(pBuf, psTokenList, NumTok, pKey = "timeZoneName", nvsVars.TimeZoneName, vfSXX) ;
					if (iRetVal >= erSUCCESS) {
						sTZ.pcDSTName = nvsVars.TimeZoneName ;
						sTSZ.pTZ = &sTZ ;				// ensure we use TZ info in future..
					}
				}
			}
		}
	}
	if (iRetVal < erSUCCESS) {
		SL_ERR("Parsing '%s' key", pKey) ;
	} else {
		if (nvsVars.TimeZoneId[0] &&
			nvsVars.TimeZoneName[0]) {
			nvsVars.fTZinfo = 1 ;
			VarsFlag |= varFLAG_TIMEZONE ;
		}
		IF_EXEC_4(debugJSON, xJsonPrintTokens, (uint8_t *) pBuf, psTokenList, NumTok, 0) ;
		IF_PRINT(debugRESULT, "New Timezone %+Z\n", &sTSZ) ;
	}
	if (psTokenList) {
		vPortFree(psTokenList) ;
	}
    return iRetVal ;
}

int32_t	xHttpGetTimeZone(void) {
	if (nvsVars.fTZinfo &&
		nvsVars.TimeZoneId[0] &&
		nvsVars.TimeZoneName[0]) {
		return erSUCCESS ;
	}
	sock_sec_t sSecure	= { 0 } ;
	sSecure.pPem		= HostInfo[hostGOOGLE].pCert ;
	sSecure.PemSize		= HostInfo[hostGOOGLE].Size ;
	http_reqres_t sReq	= { 0 } ;
	sReq.sCtx.psSec		= &sSecure ;
	sReq.sCtx.pHost		= HostInfo[hostGOOGLE].pName ;
	sReq.pcQuery		= googleAPI_TIMEZONE_QUERY ;
	sReq.hvAccept		= ctApplicationJson ;
	sReq.sfCB.on_body	= xHttpParseTimeZone  ;
#if 0
	sReq.f_debug		= 1 ;
	sReq.sCtx.d_open	= 1 ;
	sReq.sCtx.d_secure	= 1 ;
	sReq.sCtx.d_write	= 1 ;
	sReq.sCtx.d_read	= 1 ;
	sReq.sCtx.d_data	= 1 ;
	sReq.sCtx.d_eagain	= 1 ;
#endif
	return xHttpClientExecuteRequest(&sReq, nvsVars.GeoLocation[Latitude], nvsVars.GeoLocation[Longitude], xTimeStampAsSeconds(sTSZ.usecs)) ;
}

// ########################################## Elevation #############################################

int32_t	xHttpParseElevation(http_parser* psParser, const char* pBuf, size_t xLen) {
	int32_t		iRV = erFAILURE, NumTok ;
	const char * pKey = " Insufficient" ;
	jsmn_parser	sParser ;
	jsmntok_t *	psTokenList ;
	NumTok = xJsonParse((uint8_t *) pBuf, xLen, &sParser, &psTokenList) ;
	if (NumTok > 0) {								// parse Elevation
		iRV = xJsonParseKeyValue(pBuf, psTokenList, NumTok, pKey = "elevation", &nvsVars.GeoLocation[Altitude], vfFXX) ;
		if (iRV >= erSUCCESS) {					// parse Resolution
			iRV = xJsonParseKeyValue(pBuf, psTokenList, NumTok, pKey = "resolution", &nvsVars.GeoLocation[Resolution], vfFXX) ;
		}
	}
	if (iRV < erSUCCESS) {
		SL_ERR("Parsing '%s' key", pKey) ;
	} else {
		if (nvsVars.GeoLocation[Altitude]) {
			nvsVars.fGeoAlt = 1 ;
			VarsFlag |= varFLAG_ELEVATION ;
		}
		IF_PRINT(debugRESULT, "alt: %.7f res: %.7f\n", nvsVars.GeoLocation[Altitude], nvsVars.GeoLocation[Resolution]) ;
	}
	IF_EXEC_4(debugJSON, xJsonPrintTokens, (uint8_t *) pBuf, psTokenList, NumTok, 0) ;
	if (psTokenList) {
		vPortFree(psTokenList) ;
	}
    return iRV ;
}

int32_t	xHttpGetElevation(void) {
	if (nvsVars.fGeoAlt &&
		nvsVars.GeoLocation[Altitude]) {
		return erSUCCESS ;
	}
	sock_sec_t sSecure	= { 0 } ;
	sSecure.pPem		= HostInfo[hostGOOGLE].pCert ;
	sSecure.PemSize		= HostInfo[hostGOOGLE].Size ;
	http_reqres_t sReq	= { 0 } ;
	sReq.sCtx.psSec		= &sSecure ;
	sReq.sCtx.pHost		= HostInfo[hostGOOGLE].pName ;
	sReq.pcQuery		= googleAPI_ELEVATION_QUERY ;
	sReq.hvContentType	= ctUNDEFINED ;
	sReq.hvAccept		= ctApplicationJson ;
	sReq.sfCB.on_body	= xHttpParseElevation  ;
#if 0
	sReq.f_debug		= 1 ;
	sReq.sCtx.d_open	= 1 ;
	sReq.sCtx.d_secure	= 1 ;
	sReq.sCtx.d_write	= 1 ;
	sReq.sCtx.d_read	= 1 ;
	sReq.sCtx.d_data	= 1 ;
	sReq.sCtx.d_eagain	= 1 ;
#endif
	return xHttpClientExecuteRequest(&sReq, nvsVars.GeoLocation[Latitude], nvsVars.GeoLocation[Longitude]) ;
}

// ######################################## Rules download #########################################

int32_t xHttpClientRulesDownloadHandler(http_parser * psParser, const char * pBuf, size_t xLen) {
	// check if all OK to continue
	if (xHttpClientFileDownloadCheck(psParser, pBuf, xLen) == erFAILURE) {
		return erFAILURE ;
	}

	MQTTMessage	RulesMessage ;
	MessageData	RulesData ;
	RulesData.message				= &RulesMessage ;
	RulesData.message->payload		= (char *) pBuf ;
	RulesData.message->payloadlen	= xLen ;
	vMqttSubscribeRulesTEXT(&RulesData) ;
	return erSUCCESS ;
}

int32_t	xHttpClientRulesDownload(void) {
	http_reqres_t	sReq = { 0 } ;
	sReq.pcQuery		= "GET /configs/%s.cfg" ;
	sReq.sCtx.pHost		= HostInfo[nvsVars.HostCONF].pName ;
	sReq.hvContentType	= ctTextPlain ;
	sReq.hvAccept		= ctApplicationOctetStream ;
	sReq.sfCB.on_body	= xHttpClientRulesDownloadHandler ;
//	sReq.f_debug		= 1 ;
	int32_t iRetVal 	= xHttpClientExecuteRequest(&sReq, idSTA) ;
	return iRetVal ;
}

// ################################## POST IDENT info to host ######################################

int32_t	xHttpClientIdentUpload(void * pvPara) {
	http_reqres_t sReq	= { 0 } ;
	sReq.pcQuery		= "PATCH /ibuttons.dat" ;
	sReq.pcBody			= "'%m' , 'DS1990R' , 'Heavy Duty' , 'Maxim'\n" ;
	sReq.sCtx.pHost		= HostInfo[nvsVars.HostCONF].pName ;
	sReq.hvContentType	= ctApplicationOctetStream ;
//	sReq.f_append		= 1 ;
	sReq.f_debug		= 1 ;
	int32_t iRetVal 	= xHttpClientExecuteRequest(&sReq, pvPara) ;
	return iRetVal ;
}

// ################################## PUT core dump to host ########################################


int32_t xHttpClientCoredumpUploadCB(http_reqres_t * psReq) {
	int32_t	iRetVal = erFAILURE ;
	/* see https://github.com/espressif/esp-idf/issues/1650 */
	size_t xLenDone = 4 ;								// skip first 4 bytes
	IF_CPRINT(debugTRACK, "Coredump START upload %lld ", psReq->hvContentLength) ;

	while (xLenDone < psReq->hvContentLength) {			// deal with all data to be sent
		size_t xLenLeft = psReq->hvContentLength - xLenDone ;
		iRetVal = esp_partition_read((esp_partition_t *) psReq->pvArg, xLenDone, psReq->sBuf.pBuf,
									(xLenLeft > psReq->sBuf.Size) ? psReq->sBuf.Size : xLenLeft) ;
		if (iRetVal != ESP_OK) {
			SL_ERR("partition read err=0x%x (%s)", iRetVal, strerror(iRetVal)) ;
			break ;
		}
		iRetVal = xNetWrite(&psReq->sCtx, (char *) psReq->sBuf.pBuf, (xLenLeft > psReq->sBuf.Size) ? psReq->sBuf.Size : xLenLeft) ;
		IF_CPRINT(debugTRACK, ".") ;
		if (iRetVal > 0) {
			xLenDone += iRetVal ;
		} else if (psReq->sCtx.error == EAGAIN) {
			continue ;
		} else {										// transmit error or socket closed?
			SL_ERR("socket write err=0x%x (%s)", psReq->sCtx.error, strerror(psReq->sCtx.error)) ;
			break ;
		}
	}
	if (iRetVal > 0) {
		iRetVal = xNetWrite(&psReq->sCtx, (char *) "\n\n", 2) ;	// write the terminating LF's
		SL_WARN("upload done") ;
		IF_CPRINT(debugTRACK, " Coredump DONE\r\n") ;
	} else {
		IF_CPRINT(debugTRACK, " Coredump upload failed err=%d\r\n", iRetVal) ;
	}
	return iRetVal ;
}

int32_t	xHttpClientCoredumpUpload(void * pvPara) {
	sock_sec_t sSecure	= { 0 } ;
	sSecure.pPem		= HostInfo[nvsVars.HostFOTA].pCert ;
	sSecure.PemSize		= HostInfo[nvsVars.HostFOTA].Size ;
	http_reqres_t sRR	= { 0 } ;
	sRR.sCtx.psSec		= &sSecure ;
	sRR.sCtx.pHost		= HostInfo[nvsVars.HostFOTA].pName ;
	sRR.pcQuery			= "PUT /coredump/%m_%X_%X_%llu.bin" ;
	sRR.handler			= xHttpClientCoredumpUploadCB ;
	sRR.hvContentType	= ctApplicationOctetStream ;

	// for binary uploads the address and content length+type must be correct
	esp_partition_iterator_t sIter = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL) ;
	IF_myASSERT(debugRESULT, sIter != 0) ;
	const esp_partition_t *	psPart = esp_partition_get(sIter) ;
	sRR.pvArg			= (void *) psPart ;
	sRR.hvContentLength	= (uint64_t) psPart->size ;
#if 0
	sRR.f_debug			= 1 ;
	sRR.sCtx.d_open		= 1 ;
	sRR.sCtx.d_write	= 1 ;
	sRR.sCtx.d_secure	= 1 ;
	sRR.sCtx.d_level	= 2 ;
#endif
	int32_t iRetVal 	= xHttpClientExecuteRequest(&sRR, macSTA, esp_reset_reason(), DEV_FW_VER_NUM, sTSZ.usecs/MICROS_IN_SECOND) ;

	esp_partition_iterator_release(sIter) ;
	return iRetVal ;
}
