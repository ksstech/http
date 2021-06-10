/*
 * http_client.c
 * Copyright 2014-20 Andre M Maree / KSS Technologies (Pty) Ltd.
 */

#include	"hal_variables.h"
#include	"x_http_client.h"

#include	"FreeRTOS_Support.h"

#include	"parserX.h"									// parsing location & TZ requests
#include	"x_errors_events.h"
#include	"x_string_general.h"
#include	"printfx.h"
#include	"syslog.h"
#include	"systiming.h"

#include	"hal_network.h"
#include	"hal_fota.h"								// firmware download handler

#include	<string.h>
#include	<stdarg.h>

// ############################### BUILD: debug configuration options ##############################

#define	debugFLAG					0xE010

#define	debugJSON					(debugFLAG & 0x0001)
#define	debugGEOLOC					(debugFLAG & 0x0002)
#define	debugCOREDUMP				(debugFLAG & 0x0004)

#define	debugFOTA					(debugFLAG & 0x0010)

#define	debugREQUEST				(debugFLAG & 0x0100)

#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ###################################### BUILD : CONFIG definitions ###############################

#define configHTTP_BUFSIZE			4096
#define configHTTP_RX_WAIT			5000	// 500

// ########################################### macros #############################################

// https://www.iplocation.net/

/*
 * EurekAPI.com (basic free 30day, 2017/06/06)
 */
#define	configHTTP_HOST_EUREKAPI	"api.eurekapi.com"
#define	configHTTP_QUERY_EUREKAPI	"GET /iplocation/v1.8/locateip?key="keyEUREKA"&ip=%I&format=JSON&compact=Y"

/*
 * free geo-ip service definition
 */
#define	configHTTP_HOST_FREE_GEO_IP	"freegeoip.net"
#define	configHTTP_FORMAT_FREE_GEO_IP	"GET /json"

// ################################### global/public variables #####################################


// ################################### Common HTTP API functions ###################################

int32_t	xHttpBuildHeader(http_parser * psParser) {
	http_rr_t * psRR = psParser->data ;
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psParser)) ;
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psRR)) ;
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psRR->sUB.pBuf)) ;

	vuprintfx(&psRR->sUB, psRR->pcQuery, psRR->VaList) ;
	uprintfx(&psRR->sUB, " HTTP/1.1\r\n") ;
	uprintfx(&psRR->sUB, "Host: %s\r\n", psRR->sCtx.pHost) ;
	uprintfx(&psRR->sUB, "From: %s\r\n", "admin@kss.co.za") ;
	uprintfx(&psRR->sUB, "User-Agent: %s\r\n", "irmacos") ;
	if (psRR->hvAccept) {
		uprintfx(&psRR->sUB, "Accept: %s\r\n", ctValues[psRR->hvAccept]) ;
		psRR->hvAccept	= ctUNDEFINED ;
	}
	if (psRR->hvConnect) {
		uprintfx(&psRR->sUB, "Connection: %s\r\n", coValues[psRR->hvConnect]) ;
	}
	// from here on items common to requests and responses...
	if (psRR->pcBody) {
		if (psRR->hvContentType) {
			uprintfx(&psRR->sUB, "Content-Type: %s\r\n", ctValues[psRR->hvContentType]) ;
			if (psRR->hvContentType == ctApplicationOctetStream) {
				IF_myASSERT(debugREQUEST, INRANGE(1, psRR->hvContentLength, 2*MEGA, uint64_t)) ;
				/* Since the actual binary payload will only be added in the callback
				 * from xHttpClientExecuteRequest() we will only add a single "\r\n"
				 * pair here, the second added at the end of this function.
				 * The callback will be responsible for adding the final terminating "\r\n" */
				uprintfx(&psRR->sUB, "Content-Length: %d\r\n", psRR->hvContentLength) ;
				// no actual binary content added, done later...
			} else {									// currently handle json/xml/text/html here
				psRR->hvContentLength = vsprintfx(NULL, psRR->pcBody, psRR->VaList) ;	// determine body length
				uprintfx(&psRR->sUB, "Content-Length: %d\r\n\r\n", psRR->hvContentLength) ;
				vuprintfx(&psRR->sUB, psRR->pcBody, psRR->VaList) ;// add actual content
			}
		} else {
			SL_ERR(debugAPPL_PLACE) ;
		}
	}
	// add the final CR after the headers and payload, if binary payload this is 2nd "\r\n" pair
	uprintfx(&psRR->sUB, "\r\n") ;
	IF_TRACK(debugREQUEST && psRR->f_debug, "Content:\n%.*s\n", psRR->sUB.Used, psRR->sUB.pBuf) ;
	return psRR->sUB.Used ;
}

/**
 * xHttpClientExecuteRequest()
 * @param psRR
 * @return	erFAILURE or result of xHttpCommonDoParsing() being 0 or more
 */
int32_t	xHttpClientExecuteRequest(http_rr_t * psRR, va_list vArgs) {
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psRR)) ;
	IF_SYSTIMER_INIT(debugTIMING, systimerHTTP, systimerTICKS, "HTTPclnt", myMS_TO_TICKS(configHTTP_RX_WAIT/100), myMS_TO_TICKS(configHTTP_RX_WAIT)) ;
	IF_SYSTIMER_START(debugTIMING, systimerHTTP) ;
	http_parser sParser ;
	http_parser_init(&sParser, HTTP_RESPONSE) ;			// clear all parser fields/values
	sParser.data = psRR ;

	// setup the uubuf_t structure for printing
	xUBufCreate(&psRR->sUB, NULL, psRR->sUB.Size, 0) ;

	psRR->VaList = vArgs ;
	int32_t xLen = xHttpBuildHeader(&sParser) ;

	psRR->sCtx.type				= SOCK_STREAM ;
	psRR->sCtx.sa_in.sin_family	= AF_INET ;
	if (psRR->sCtx.sa_in.sin_port	== 0) {
		psRR->sCtx.sa_in.sin_port	= htons(psRR->sCtx.psSec ? IP_PORT_HTTPS : IP_PORT_HTTP) ;
	}
	int32_t iRV = xNetOpen(&psRR->sCtx) ;
	if (iRV == erSUCCESS) {								// if socket=open, write request
		iRV = xNetWrite(&psRR->sCtx, psRR->sUB.pBuf, xLen) ;
		if (iRV > 0) {									// successfully written some (or all)
			if (psRR->hvContentType == ctApplicationOctetStream) {
				iRV = psRR->hdlr_req(psRR) ;			// should return same as xNetWrite()
			}
			if (iRV > 0) {								// now do the actual read
				iRV = xNetReadBlocks(&psRR->sCtx, psRR->sUB.pBuf, psRR->sUB.Size, configHTTP_RX_WAIT) ;
				if (iRV > 0) {							// actually read something
					psRR->sUB.Used = iRV ;
					iRV = xHttpCommonDoParsing(&sParser) ;	// return erFAILURE or some 0+ number
				} else {
					IF_TRACK(debugREQUEST, " nothing read ie to parse\n") ;
					iRV = erFAILURE ;
				}
			} else {
				IF_TRACK(debugREQUEST, " nothing written (by handler) so can't expect to read\n") ;
				iRV = erFAILURE ;
			}
		} else {
			IF_TRACK(debugREQUEST, " no header written, so can't expect to read\n") ;
			iRV = erFAILURE ;
		}
	} else {
		IF_TRACK(debugREQUEST, " could not open connection (%d)\n", iRV) ;
		iRV = erFAILURE ;
	}
	xNetClose(&psRR->sCtx) ;							// close the socket connection if still open...
	vUBufDestroy(&psRR->sUB) ;							// return memory allocated
	IF_SYSTIMER_STOP(debugTIMING, systimerHTTP) ;
	return iRV ;
}

int32_t	xHttpRequest(pci8_t pHost, pci8_t pQuery, const void * pvBody,
		pci8_t pcCert, size_t szCert, void * OnBodyCB, uint32_t DataSize,
		uint32_t hvValues, uint16_t BufSize, xnet_debug_t Debug, void * pvArg, ...) {
	IF_myASSERT(debugREQUEST, halCONFIG_inFLASH(OnBodyCB)) ;
	http_rr_t sRR		= { 0 } ;
	sock_sec_t sSecure	= { 0 } ;				// LEAVE here else pcCert/szCert gets screwed
	sRR.sCtx.pHost		= pHost ;
	sRR.pcQuery			= pQuery ;
	sRR.pVoid			= pvBody ;
	sRR.sfCB.on_body	= OnBodyCB ? (http_data_cb) OnBodyCB : xHttpCommonMessageBodyHandler ;
	sRR.hvContentLength	= (uint64_t) DataSize ;
	sRR.hvValues		= hvValues ;
	sRR.sUB.Size		= BufSize ? BufSize : configHTTP_BUFSIZE ;
	sRR.pvArg			= pvArg ;
	IF_CPRINT(debugREQUEST, "H='%s'  Q='%s'  B=", sRR.sCtx.pHost, sRR.pcQuery) ;
	IF_CPRINT(debugREQUEST, sRR.hvContentType == ctApplicationOctetStream ? "%p" : "'%s'", sRR.pVoid) ;
	IF_CPRINT(debugREQUEST, "  cb=%p  hv=%-I\n", sRR.sfCB.on_body, sRR.hvValues) ;
	IF_myASSERT(debugREQUEST, sRR.hvContentType != ctUNDEFINED) ;

	if (pcCert) {
		sRR.sCtx.psSec	= &sSecure ;
		sSecure.pcCert	= pcCert ;
		sSecure.szCert	= szCert ;
	}
	if (Debug.u32) {
		sRR.f_debug			= Debug.http ;
		sRR.sCtx.d_open		= Debug.open ;
		sRR.sCtx.d_write	= Debug.write ;
		sRR.sCtx.d_read		= Debug.read ;
		sRR.sCtx.d_data		= Debug.data ;
		sRR.sCtx.d_eagain	= Debug.eagain ;
		if (pcCert) {
			sRR.sCtx.psSec->Verify	= Debug.verify ;
			sRR.sCtx.d_secure		= Debug.secure ;
			sRR.sCtx.d_level		= Debug.level ;
		}
	}
	va_list vArgs ;
	va_start(vArgs, pvArg) ;
	int32_t iRV = xHttpClientExecuteRequest(&sRR, vArgs) ;
	va_end(vArgs) ;
	return iRV ;
}

// ###################################### WEATHER support ##########################################

int32_t xHttpGetWeather(void) {
	const char caQuery[] = "GET /data/2.5/forecast/?q=Johannesburg,ZA&APPID=cf177bb6e86c95045841c63e99ad2ff4" ;
	return xHttpRequest("api.openweathermap.org", caQuery, NULL,
			NULL, 0, NULL, 0,
			httpHDR_VALUES(ctTextPlain,0,0,0),
			16384, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL) ;
}

// ################################### How's my SSL support ########################################

int32_t	xHttpHowsMySSL(void) {
	return xHttpRequest("www.howsmyssl.com", "GET /a/check", NULL,
			HostInfo[sNVSvars.HostFOTA].pcCert, HostInfo[sNVSvars.HostFOTA].szCert, NULL, 0,
			httpHDR_VALUES(ctTextPlain,0,0,0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL) ;
}

// ####################################### Bad SSL support #########################################

int32_t	xHttpBadSSL(void) {
	return xHttpRequest("www.badssl.com", "GET /dashboard", NULL,
			HostInfo[sNVSvars.HostFOTA].pcCert, HostInfo[sNVSvars.HostFOTA].szCert, NULL, 0,
			httpHDR_VALUES(ctTextPlain,0,0,0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL) ;
}

// ################################# Firmware Over The Air support #################################

int32_t	xHttpClientFileDownloadCheck(http_parser * psParser) {
	http_rr_t * psRR = psParser->data ;
	int32_t iRV = erFAILURE ;
	if (psParser->status_code != HTTP_STATUS_OK) {
		IF_TRACK(debugFOTA, "'%s' not found\n", psRR->pvArg) ;
	} else if (psRR->hvContentLength == 0ULL) {
		SL_ERR("'%s' invalid size (%llu)", psRR->pvArg, psRR->hvContentLength) ;
	} else if (psRR->hvContentType != ctApplicationOctetStream) {
		SL_ERR("'%s' invalid content (%d/%s)", psRR->pvArg, psRR->hvContentType, ctValues[psRR->hvContentType]) ;
	} else if (psRR->hvConnect == coClose) {
		SL_ERR("Connection closed unexpectedly") ;
	} else {
		iRV = erSUCCESS ;
	}
	return iRV ;
}

/**
 * xHttpClientCheckFOTA() -
 * @return	1 if valid upgrade file exists
 * 			erSUCCESS if no newer upgrade file exists
 * 			erFAILURE if file not found/empty file/invalid content/connection closed
 */
int32_t	xHttpClientCheckFOTA(http_parser * psParser, const char * pBuf, size_t xLen) {
	if (xHttpClientFileDownloadCheck(psParser) == erFAILURE) {
		return erFAILURE ;
	}
	http_rr_t * psRR = psParser->data ;
	/* BuildSeconds			: halfway(?) time of running FW
	 * hvLastModified		: creation time of available FW
	 * fotaMIN_DIF_SECONDS	: Required MIN difference (hvLastModified - BuildSeconds)
	 *						: How much later must FW be to be considered new? */
	#define	fotaMIN_DIF_SECONDS					120
	int32_t i32Diff = psRR->hvLastModified - BuildSeconds - fotaMIN_DIF_SECONDS ;
	IF_TRACK(debugFOTA, "'%s' found  %r vs %r  Diff=%d  FW %snewer\n", psRR->pvArg,
						psRR->hvLastModified, BuildSeconds, i32Diff, i32Diff < 0 ? "NOT " : "") ;
	if (i32Diff < 0) {
		return erSUCCESS ;
	}
	xRtosSetStatus(flagAPP_RESTART) ;
	return 1 ;
}

int32_t xHttpClientPerformFOTA(http_parser * psParser, const char * pBuf, size_t xLen) {
	int32_t iRV = xHttpClientCheckFOTA(psParser, pBuf, xLen) ;
	if (iRV != 1) {
		return iRV ;
	}
	xRtosClearStatus(flagAPP_RESTART) ;
	fota_info_t	sFI ;
	iRV = halFOTA_Begin(&sFI) ;
	if (iRV < erSUCCESS) {
		return erFAILURE ;
	}
	IF_TRACK(debugFOTA, "OTA begin OK\n") ;

	sFI.pBuf	= (void *) pBuf ;
	sFI.xLen	= xLen ;
	http_rr_t * psReq = psParser->data ;
	size_t	xLenDone = 0, xLenFull = psReq->hvContentLength ;
	IF_SYSTIMER_INIT(debugTIMING, systimerFOTA, systimerTICKS, "halFOTA", myMS_TO_TICKS(configHTTP_RX_WAIT/10), myMS_TO_TICKS(configHTTP_RX_WAIT)) ;

	while (xLen) {										// deal with all received packets
		iRV = halFOTA_Write(&sFI) ;
		if (iRV != ESP_OK) {
			break ;
		}
		xLenDone += sFI.xLen ;
		IF_CPRINT(debugFOTA, "%d%% (%d)\r", (xLenDone * 100)/xLenFull, xLenDone) ;
		if (xLenDone == xLenFull) {						// if all done
			IF_CPRINT(debugFOTA, "\n") ;
			break ;										// get out...
		}
		IF_SYSTIMER_START(debugTIMING, systimerFOTA) ;
		iRV = xNetReadBlocks(&psReq->sCtx, (char *) (sFI.pBuf = psReq->sUB.pBuf), psReq->sUB.Size, configHTTP_RX_WAIT) ;
		IF_SYSTIMER_STOP(debugTIMING, systimerFOTA) ;
		if (iRV > 0) {
			sFI.xLen = iRV ;
		} else if (psReq->sCtx.error == EAGAIN) {
			continue ;
		} else {
			sFI.iRV = iRV ;								// save for halFOTA_End() reuse
			break ;										// no need for error reporting, already done in xNetRead()
		}
	}

	IF_SYSTIMER_SHOW_NUM(debugTIMING, systimerFOTA) ;
	IF_TRACK(debugFOTA, "Wrote %u/%u from '%s/%s'\n", xLenDone, xLenFull, psReq->sCtx.pHost, psReq->pvArg) ;

	iRV = halFOTA_End(&sFI) ;
	if (iRV == erSUCCESS && sFI.iRV == ESP_OK) {
		xRtosSetStatus(flagAPP_RESTART) ;
	}
	return sFI.iRV ;
}

int32_t	xHttpClientFirmwareUpgrade(void * pvPara, bool bCheck) {
	int32_t iRV = xHttpRequest(HostInfo[sNVSvars.HostFOTA].pName, "GET /firmware/%s.bin", NULL,
			HostInfo[sNVSvars.HostFOTA].pcCert, HostInfo[sNVSvars.HostFOTA].szCert,
			bCheck == CHECK ? xHttpClientCheckFOTA : xHttpClientPerformFOTA, 0,
			httpHDR_VALUES(ctTextPlain, ctApplicationOctetStream, coKeepAlive, 0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,4), pvPara, pvPara) ;
	return iRV ;
}

/**
 * xHttpClientCheckUpgrades()
 * @		step through multiple FW upgrade options till a valid option found or until all options done.
 * @return
 */
int32_t xHttpClientCheckUpgrades(bool bCheck) {
	/* To create a hierarchy of firmware upgrades, we need to define a descending order:
	 * #1 would be MAC based "1234567890ab.bin" hence 100% specific
	 * #2 would be "[site-token].bin"
	 * #3 would define a level to accommodate a specific client/tenant
	 * #4 would be the broadest "[device-specification-token].bin"
	 */
	int32_t iRV = xHttpClientFirmwareUpgrade((void *) idSTA, bCheck) ;
#if 0
	if (bRtosCheckStatus(flagAPP_RESTART) == 0) {
		iRV = xHttpClientFirmwareUpgrade((void *) mqttSITE_TOKEN, bCheck) ;
	}
#endif
	if (bRtosCheckStatus(flagAPP_RESTART) == 0) {
		iRV = xHttpClientFirmwareUpgrade((void *) halDEV_UUID, bCheck) ;
	}
	xRtosClearStatus(flagAPP_UPGRADE) ;				// all options exhausted
	if (bCheck == PERFORM) {
		xSyslog(SL_MOD2LOCAL(iRV == erFAILURE ? SL_SEV_ERROR : SL_SEV_NOTICE),
				"FW Upgrade %s", iRV == erFAILURE ? "Failed" : "Done") ;
	}
	return iRV ;
}

// ########################################## Location #############################################

#define GooglePEM		"-----BEGIN CERTIFICATE-----\n"						\
	"MIIDIDCCAomgAwIBAgIENd70zzANBgkqhkiG9w0BAQUFADBOMQswCQYDVQQGEwJV\n"	\
	"UzEQMA4GA1UEChMHRXF1aWZheDEtMCsGA1UECxMkRXF1aWZheCBTZWN1cmUgQ2Vy\n"	\
	"dGlmaWNhdGUgQXV0aG9yaXR5MB4XDTk4MDgyMjE2NDE1MVoXDTE4MDgyMjE2NDE1\n"	\
	"MVowTjELMAkGA1UEBhMCVVMxEDAOBgNVBAoTB0VxdWlmYXgxLTArBgNVBAsTJEVx\n"	\
	"dWlmYXggU2VjdXJlIENlcnRpZmljYXRlIEF1dGhvcml0eTCBnzANBgkqhkiG9w0B\n"	\
	"AQEFAAOBjQAwgYkCgYEAwV2xWGcIYu6gmi0fCG2RFGiYCh7+2gRvE4RiIcPRfM6f\n"	\
	"BeC4AfBONOziipUEZKzxa1NfBbPLZ4C/QgKO/t0BCezhABRP/PvwDN1Dulsr4R+A\n"	\
	"cJkVV5MW8Q+XarfCaCMczE1ZMKxRHjuvK9buY0V7xdlfUNLjUA86iOe/FP3gx7kC\n"	\
	"AwEAAaOCAQkwggEFMHAGA1UdHwRpMGcwZaBjoGGkXzBdMQswCQYDVQQGEwJVUzEQ\n"	\
	"MA4GA1UEChMHRXF1aWZheDEtMCsGA1UECxMkRXF1aWZheCBTZWN1cmUgQ2VydGlm\n"	\
	"aWNhdGUgQXV0aG9yaXR5MQ0wCwYDVQQDEwRDUkwxMBoGA1UdEAQTMBGBDzIwMTgw\n"	\
	"ODIyMTY0MTUxWjALBgNVHQ8EBAMCAQYwHwYDVR0jBBgwFoAUSOZo+SvSspXXR9gj\n"	\
	"IBBPM5iQn9QwHQYDVR0OBBYEFEjmaPkr0rKV10fYIyAQTzOYkJ/UMAwGA1UdEwQF\n"	\
	"MAMBAf8wGgYJKoZIhvZ9B0EABA0wCxsFVjMuMGMDAgbAMA0GCSqGSIb3DQEBBQUA\n"	\
	"A4GBAFjOKer89961zgK5F7WF0bnj4JXMJTENAKaSbn+2kmOeUJXRmm/kEd5jhW6Y\n"	\
	"7qj/WsjTVbJmcVfewCHrPSqnI0kBBIZCe/zuf6IWUrVnZ9NA2zsmWLIodz2uFHdh\n"	\
	"1voqZiegDfqnc1zqcPGUIWVEX/r87yloqaKHee9570+sB3c4\n"					\
	"-----END CERTIFICATE-----\n"

const char CertGoogle[] = GooglePEM ;
const size_t SizeGoogle = sizeof(GooglePEM) ;

int32_t	xHttpParseGeoLoc(http_parser * psParser, const char * pcBuf, size_t xLen) {
	int32_t	iRV = erFAILURE, NumTok ;
	const char * pKey = " Insufficient" ;
	jsmn_parser	sParser ;
	jsmntok_t *	psTokenList ;
	NumTok = xJsonParse(pcBuf, xLen, &sParser, &psTokenList) ;
	if (NumTok > 0) {									// parse Latitude
		iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "lat", &sNVSvars.GeoLocation[geoLAT], vfFXX) ;
		if (iRV >= erSUCCESS) {							// parse Longitude
			iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "lng", &sNVSvars.GeoLocation[geoLON], vfFXX) ;
			if (iRV >= erSUCCESS) {						// parse accuracy
				iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "accuracy", &sNVSvars.GeoLocation[geoACC], vfFXX) ;
			}
		}
	}
	if (iRV >= erSUCCESS && sNVSvars.GeoLocation[geoLAT] && sNVSvars.GeoLocation[geoLON]) {
		sNVSvars.fGeoLoc = 1 ;
		VarsFlag |= varFLAG_LOCATION ;
		SL_INFO("lat=%.7f  lng=%.7f  acc=%.7f",
				sNVSvars.GeoLocation[geoLAT], sNVSvars.GeoLocation[geoLON], sNVSvars.GeoLocation[geoACC]) ;
		IF_EXEC_4(debugJSON, xJsonPrintTokens, pcBuf, psTokenList, NumTok, 0) ;
	} else {
		SL_ERR("Error parsing '%s' key", pKey) ;
	}
	if (psTokenList) {
		free(psTokenList) ;
	}
    return iRV ;
}

#ifndef keyGOOGLE
	#define	keyGOOGLE		"fakegoogle"
#endif

int32_t	xHttpGetLocation(void) {
	if (sNVSvars.fGeoLoc) {
		return erSUCCESS ;
	}
	const char caQuery[] = "POST /geolocation/v1/geolocate?key="keyGOOGLE ;
	int32_t iRV = xHttpRequest("www.googleapis.com", caQuery, "{ }\r\n",
			&CertGoogle[0], SizeGoogle, xHttpParseGeoLoc, 0,
			httpHDR_VALUES(ctApplicationJson, ctApplicationJson, 0, 0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,4), NULL) ;
	return iRV ;
}

// ##################################### TIMEZONE support ##########################################

/*
 * Google TimeZone API
 *		https://developers.google.com/maps/documentation/timezone/intro#Requests
 * Example:
 * 		https://maps.googleapis.com/maps/api/timezone/json?location=38.908133,-77.047119&timestamp=1458000000&key=YOUR_API_KEY
 */
int32_t	xHttpParseTimeZone(http_parser * psParser, const char * pcBuf, size_t xLen) {
	jsmn_parser	sParser ;
	jsmntok_t *	psTokenList ;
	int32_t		NumTok, iRV = erFAILURE ;
	const char * pKey = " Insufficient" ;
	NumTok = xJsonParse(pcBuf, xLen, &sParser, &psTokenList) ;
	if (NumTok > 0) {
		x32_t	xVal ;
		iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "dstOffset", &xVal.i32, vfIXX) ;
		if (iRV >= erSUCCESS) {
			sTZ.daylight = sNVSvars.daylight = xVal.i32 ;					// convert i32 -> i16 & store
			iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "rawOffset", &xVal.i32, vfIXX) ;
			if (iRV >= erSUCCESS) {
				sTZ.timezone = sNVSvars.timezone = xVal.i32 ;				// store value
				iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "timeZoneId", sNVSvars.TimeZoneId, vfSXX) ;
				if (iRV >= erSUCCESS) {
					iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "timeZoneName", sNVSvars.TimeZoneName, vfSXX) ;
				}
			}
		}
	}
	if (iRV >= erSUCCESS && sNVSvars.TimeZoneId[0] && sNVSvars.TimeZoneName[0]) {
		sNVSvars.fGeoTZ = 1 ;
		VarsFlag |= varFLAG_TIMEZONE ;
		SL_INFO("%+Z(%s)", &sTSZ, sTSZ.pTZ->pcTZName) ;
		IF_EXEC_4(debugJSON, xJsonPrintTokens, pcBuf, psTokenList, NumTok, 0) ;
	} else {
		SL_ERR("Error parsing '%s' key", pKey) ;
	}
	if (psTokenList) {
		free(psTokenList) ;
	}
    return iRV ;
}

int32_t	xHttpGetTimeZone(void) {
	if (sNVSvars.fGeoTZ) {
		sTZ.daylight	= sNVSvars.daylight ;
		sTZ.timezone	= sNVSvars.timezone ;
		return erSUCCESS ;
	}
	char const * caQuery = "GET /maps/api/timezone/json?location=%.7f,%.7f&timestamp=%d&key="keyGOOGLE ;
	int32_t iRV = xHttpRequest("maps.googleapis.com", caQuery, NULL,
			CertGoogle, sizeof(CertGoogle), xHttpParseTimeZone, 0,
			httpHDR_VALUES(ctTextPlain, ctApplicationJson, 0, 0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL,
			sNVSvars.GeoLocation[geoLAT], sNVSvars.GeoLocation[geoLON]) ;
	return iRV ;
}

// ########################################## Elevation #############################################

/*
 * Google elevation API
 *		https://developers.google.com/maps/documentation/elevation/start
 * Example:
 *		https://maps.googleapis.com/maps/api/elevation/json?locations=39.7391536,-104.9847034&key=API_KEY
 */
int32_t	xHttpParseElevation(http_parser * psParser, const char* pcBuf, size_t xLen) {
	int32_t		iRV = erFAILURE ;
	const char * pKey = " Insufficient" ;
	jsmn_parser	sParser ;
	jsmntok_t *	psTokenList ;
	int32_t NumTok = xJsonParse(pcBuf, xLen, &sParser, &psTokenList) ;
	if (NumTok > 0) {									// parse Elevation
		iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "elevation", &sNVSvars.GeoLocation[geoALT], vfFXX) ;
		if (iRV >= erSUCCESS) {							// parse Resolution
			iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "resolution", &sNVSvars.GeoLocation[geoRES], vfFXX) ;
		}
	}
	if (iRV >= erSUCCESS && sNVSvars.GeoLocation[geoALT]) {
		sNVSvars.fGeoAlt = 1 ;
		VarsFlag |= varFLAG_ELEVATION ;
		SL_INFO("alt=%.7f  res=%.7f", sNVSvars.GeoLocation[geoALT], sNVSvars.GeoLocation[geoRES]) ;
		IF_EXEC_4(debugJSON, xJsonPrintTokens, pcBuf, psTokenList, NumTok, 0) ;
	} else {
		SL_ERR("Error parsing '%s' key", pKey) ;
	}
	if (psTokenList) {
		free(psTokenList) ;
	}
    return iRV ;
}

int32_t	xHttpGetElevation(void) {
	if (sNVSvars.fGeoAlt) {
		return erSUCCESS ;
	}
	const char caQuery[] = "GET /maps/api/elevation/json?locations=%.7f,%.7f&key="keyGOOGLE ;
	int32_t iRV = xHttpRequest("maps.googleapis.com", caQuery, NULL,
			CertGoogle, sizeof(CertGoogle), xHttpParseElevation, 0,
			httpHDR_VALUES(ctTextPlain, ctApplicationJson, 0, 0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL,
			sNVSvars.GeoLocation[geoLAT], sNVSvars.GeoLocation[geoLON]) ;
	return iRV ;
}

// ############################## Combined GeoLoc dependent info ###################################

int32_t	xHttpClientCheckGeoLoc(void) {
	int32_t iRV = xHttpGetLocation() ;
	if (iRV > erFAILURE) {								// Elevation & TimeZone require Location
		iRV = xHttpGetElevation() ;
		iRV = xHttpGetTimeZone() ;
	}
	return iRV ;
}

// ###################################### Various gateways #########################################

#ifndef	tokenPUSHOVER
	#define	tokenPUSHOVER "faketoken"
#endif

#ifndef	userPUSHOVER
	#define	userPUSHOVER "fakeuser"
#endif

int32_t	xHttpClientPushOver(const char * pcMess, uint32_t u32Val) {
	const char caBody[] = "token="tokenPUSHOVER "&user="userPUSHOVER "&title=%U&message=%U%%40%u" ;
	int32_t iRV = xHttpRequest("api.pushover.net", "POST /1/messages.json", caBody,
			CertGoogle, sizeof(CertGoogle), NULL, 0,
			httpHDR_VALUES(ctApplicationXwwwFormUrlencoded, ctApplicationJson, 0, 0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL,
			nameSTA, pcMess, u32Val) ;
	return iRV ;
}

// ######################################## Rules download #########################################

#if 0
#include	"sitewhere-text.h"							// for vMqttSubscribeRulesTEXT()
int32_t xHttpClientRulesDownloadHandler(http_parser * psParser, const char * pBuf, size_t xLen) {
	if (xHttpClientFileDownloadCheck(psParser) == erFAILURE)
		return erFAILURE ;
	MQTTMessage	RulesMessage ;
	MessageData	RulesData ;
	RulesData.message				= &RulesMessage ;
	RulesData.message->payload		= (char *) pBuf ;
	RulesData.message->payloadlen	= xLen ;
	vMqttSubscribeRulesTEXT(&RulesData) ;
	return erSUCCESS ;
}

int32_t	xHttpClientRulesDownload(void) {
	return xHttpRequest(HostInfo[sNVSvars.HostCONF].pName,
			"GET /configs/%s.cfg", NULL, NULL,  xHttpClientRulesDownloadHandler, 0,
			httpHDR_VALUES(ctTextPlain, ctApplicationOctetStream, 0, 0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL,
			idSTA ) ;
}
#endif

// ################################## POST IDENT info to host ######################################

/**
 * xHttpClientIdentUpload - Upload single DS1990 tag info to cloud
 * @param[in]	pointer to tag ROM ID string
 */
int32_t	xHttpClientIdentUpload(void * psRomID) {
	return xHttpRequest(HostInfo[sNVSvars.HostCONF].pName, "PATCH /ibuttons.dat",
			"'%m' , 'DS1990R' , 'Heavy Duty' , 'Maxim'\n",
			NULL, 0, NULL, 0, httpHDR_VALUES(ctTextPlain, 0, 0, 0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL,
			psRomID) ;
}

// ################################## PUT core dump to host ########################################

int32_t xHttpClientCoredumpUploadCB(http_rr_t * psReq) {
	int32_t	iRV = erFAILURE ;
	/* see https://github.com/espressif/esp-idf/issues/1650 */
	size_t		xNow, xLeft, xDone = 0 ;
	IF_TRACK(debugCOREDUMP, "Coredump START upload %lld\n", psReq->hvContentLength) ;

	while (xDone < psReq->hvContentLength) {			// deal with all data to be sent
		xLeft	= psReq->hvContentLength - xDone ;
		xNow	= (xLeft > psReq->sUB.Size) ? psReq->sUB.Size : xLeft ;
		IF_TRACK(debugCOREDUMP, "Start:%u  Write:%u  Left:%u\n", xDone, xNow, xLeft) ;
		iRV = esp_partition_read((esp_partition_t *) psReq->pvArg, xDone, psReq->sUB.pBuf, xNow) ;
		if (iRV != ESP_OK) {
			SL_ERR("flash read err=0x%x (%s)", iRV, esp_err_to_name(iRV)) ;
			return -iRV ;
		}
		iRV = xNetWrite(&psReq->sCtx, (char *) psReq->sUB.pBuf, (xLeft > psReq->sUB.Size) ? psReq->sUB.Size : xLeft) ;
		if (iRV > 0) {
			xDone += iRV ;
		} else if (psReq->sCtx.error == EAGAIN) {
			continue ;
		} else {										// transmit error or socket closed?
			SL_ERR("net write err=0x%x (%s)", psReq->sCtx.error, strerror(psReq->sCtx.error)) ;
			break ;
		}
	}
	if (iRV > 0) {
		iRV = xNetWrite(&psReq->sCtx, (char *) "\r\n", 2) ;	// write the terminating CR/LF
		iRV = xDone + 2 ;
	}
	return iRV ;
}

typedef struct {
    uint32_t data_len;  // data length
    uint32_t version;   // core dump struct version
    uint32_t tasks_num; // number of tasks
    uint32_t tcb_sz;    // size of TCB
} cd_hdr_t;

#if		defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_BIN)
	const char caQuery[] = "PUT /coredump/%m_%X_%X_%llu.bin" ;
#elif	defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
	const char caQuery[] = "PUT /coredump/%m_%X_%X_%llu.elf" ;
#endif

int32_t	xHttpClientCoredumpUpload(void * pvPara) {
	// for binary uploads the address and content length+type must be correct
	esp_partition_iterator_t sIter ;
	sIter = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL) ;
	IF_myASSERT(debugCOREDUMP, sIter != 0) ;

	const esp_partition_t *	psPart = esp_partition_get(sIter) ;
	IF_myASSERT(debugCOREDUMP, psPart != 0) ;

	cd_hdr_t	sCDhdr ;
	int32_t iRV = esp_partition_read(psPart, 0, &sCDhdr, sizeof(sCDhdr)) ;
	IF_TRACK(debugCOREDUMP, "L=%d  T=%u  TCB=%u  V=%-I\n", sCDhdr.data_len, sCDhdr.tasks_num, sCDhdr.tcb_sz, sCDhdr.version) ;

	if (iRV != ESP_OK || (sCDhdr.data_len == sCDhdr.tasks_num && sCDhdr.tcb_sz == sCDhdr.version)) {
		SL_ALRT("Error =%d (%s) L=%d  T=%u  TCB=%u  V=%-I", iRV, esp_err_to_name(iRV), sCDhdr.data_len, sCDhdr.tasks_num, sCDhdr.tcb_sz, sCDhdr.version) ;
		iRV = erFAILURE ;
	}

	if (iRV == ESP_OK) {
		iRV = xHttpRequest(HostInfo[sNVSvars.HostCONF].pName, caQuery,
				xHttpClientCoredumpUploadCB,
				HostInfo[sNVSvars.HostFOTA].pcCert,HostInfo[sNVSvars.HostFOTA].szCert,
				NULL, sCDhdr.data_len,
				httpHDR_VALUES(ctApplicationOctetStream, 0, 0, 0),
				0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), (void *) psPart,
				macSTA, esp_reset_reason(), DEV_FW_VER_NUM, sTSZ.usecs/MICROS_IN_SECOND) ;
		SL_WARN("iRV=%d  L=%u  T=%u  TCB=%u  V=%-I", iRV, sCDhdr.data_len, sCDhdr.tasks_num, sCDhdr.tcb_sz, sCDhdr.version) ;
	}
	esp_partition_iterator_release(sIter) ;
	return iRV ;
}
