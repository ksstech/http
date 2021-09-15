/*
 * http_client.c
 * Copyright 2014-20 Andre M Maree / KSS Technologies (Pty) Ltd.
 */

#include	"hal_variables.h"
#include	"x_http_client.h"

#include	"FreeRTOS_Support.h"
#include	"parserX.h"									// parsing location & TZ requests
#include	"printfx.h"
#include	"syslog.h"
#include	"systiming.h"

#include	"x_string_general.h"
#include	"x_errors_events.h"

#include	"hal_network.h"
#include	"hal_fota.h"								// firmware download handler

// ############################### BUILD: debug configuration options ##############################

#define	debugFLAG					0xF000

#define	debugJSON					(debugFLAG & 0x0001)
#define	debugGEOLOC					(debugFLAG & 0x0002)
#define	debugCOREDUMP				(debugFLAG & 0x0004)

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

int	xHttpBuildHeader(http_parser * psParser) {
	http_rr_t * psRR = psParser->data ;
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psParser)) ;
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psRR)) ;
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psRR->sUB.pBuf)) ;

	vuprintfx(&psRR->sUB, psRR->pcQuery, psRR->VaList) ;
	uprintfx(&psRR->sUB, " HTTP/1.1\r\nHost: %s\r\nFrom: admin@kss.co.za\r\nUser-Agent: irmacos\r\n", psRR->sCtx.pHost) ;
	if (psRR->hvAccept) {
		uprintfx(&psRR->sUB, "Accept: %s\r\n", ctValues[psRR->hvAccept]) ;
		psRR->hvAccept	= ctUNDEFINED ;
	}
	if (psRR->hvConnect) uprintfx(&psRR->sUB, "Connection: %s\r\n", coValues[psRR->hvConnect]);
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
		} else SL_ERR(debugAPPL_PLACE) ;
	}
	// add the final CR after the headers and payload, if binary payload this is 2nd "\r\n" pair
	uprintfx(&psRR->sUB, "\r\n") ;
	IF_PRINT(debugREQUEST && psRR->f_debug, "Content:\n%.*s\n", psRR->sUB.Used, psRR->sUB.pBuf) ;
	return psRR->sUB.Used ;
}

/**
 * xHttpClientExecuteRequest()
 * @param psRR
 * @return	erFAILURE or result of xHttpCommonDoParsing() being 0 or more
 */
int	xHttpClientExecuteRequest(http_rr_t * psRR, va_list vArgs) {
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psRR)) ;
	IF_SYSTIMER_INIT(debugTIMING, stHTTP, stMILLIS, "HTTPclnt", configHTTP_RX_WAIT/100, configHTTP_RX_WAIT) ;
	IF_SYSTIMER_START(debugTIMING, stHTTP) ;
	http_parser sParser ;
	http_parser_init(&sParser, HTTP_RESPONSE) ;			// clear all parser fields/values
	sParser.data = psRR ;

	// setup the uubuf_t structure for printing
	xUBufCreate(&psRR->sUB, NULL, psRR->sUB.Size, 0) ;

	psRR->VaList = vArgs ;
	int xLen = xHttpBuildHeader(&sParser) ;

	psRR->sCtx.type				= SOCK_STREAM ;
	psRR->sCtx.sa_in.sin_family	= AF_INET ;
	if (psRR->sCtx.sa_in.sin_port == 0)
		psRR->sCtx.sa_in.sin_port = htons(psRR->sCtx.psSec ? IP_PORT_HTTPS : IP_PORT_HTTP) ;
	int iRV = xNetOpen(&psRR->sCtx) ;
	if (iRV == erSUCCESS) {								// if socket=open, write request
		iRV = xNetWrite(&psRR->sCtx, psRR->sUB.pBuf, xLen) ;
		if (iRV > 0) {									// successfully written some (or all)
			if (psRR->hvContentType == ctApplicationOctetStream) iRV = psRR->hdlr_req(psRR) ;			// should return same as xNetWrite()
			if (iRV > 0) {								// now do the actual read
				iRV = xNetReadBlocks(&psRR->sCtx, psRR->sUB.pBuf, psRR->sUB.Size, configHTTP_RX_WAIT) ;
				if (iRV > 0) {							// actually read something
					psRR->sUB.Used = iRV ;
					iRV = xHttpCommonDoParsing(&sParser) ;	// return erFAILURE or some 0+ number
				} else {
					IF_PRINT(debugREQUEST, " nothing read ie to parse\n") ;
					iRV = erFAILURE ;
				}
			} else {
				IF_PRINT(debugREQUEST, " nothing written (by handler) so can't expect to read\n") ;
				iRV = erFAILURE ;
			}
		} else {
			IF_PRINT(debugREQUEST, " no header written, so can't expect to read\n") ;
			iRV = erFAILURE ;
		}
	} else {
		IF_PRINT(debugREQUEST, " open/connect error (%d)\n", iRV) ;
		iRV = erFAILURE ;
	}
	xNetClose(&psRR->sCtx) ;							// close the socket connection if still open...
	vUBufDestroy(&psRR->sUB) ;							// return memory allocated
	IF_SYSTIMER_STOP(debugTIMING, stHTTP) ;
	return iRV ;
}

int	xHttpRequest(pci8_t pHost, pci8_t pQuery, const void * pvBody,
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

int xHttpGetWeather(void) {
	const char caQuery[] = "GET /data/2.5/forecast/?q=Johannesburg,ZA&APPID=cf177bb6e86c95045841c63e99ad2ff4" ;
	return xHttpRequest("api.openweathermap.org", caQuery, NULL,
			NULL, 0, NULL, 0,
			httpHDR_VALUES(ctTextPlain,0,0,0),
			16384, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL) ;
}

// ################################### How's my SSL support ########################################

int	xHttpHowsMySSL(void) {
	return xHttpRequest("www.howsmyssl.com", "GET /a/check", NULL,
			HostInfo[sNVSvars.HostFOTA].pcCert, HostInfo[sNVSvars.HostFOTA].szCert, NULL, 0,
			httpHDR_VALUES(ctTextPlain,0,0,0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL) ;
}

// ####################################### Bad SSL support #########################################

int	xHttpBadSSL(void) {
	return xHttpRequest("www.badssl.com", "GET /dashboard", NULL,
			HostInfo[sNVSvars.HostFOTA].pcCert, HostInfo[sNVSvars.HostFOTA].szCert, NULL, 0,
			httpHDR_VALUES(ctTextPlain,0,0,0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL) ;
}

// ################################# Firmware Over The Air support #################################

int	xHttpClientFileDownloadCheck(http_parser * psParser) {
	http_rr_t * psRR = psParser->data ;
	int iRV = erFAILURE ;
	if (psParser->status_code != HTTP_STATUS_OK) {
		IF_PRINT(debugTRACK && ioB1GET(ioFOTA), "'%s' not found\n", psRR->pvArg) ;
	} else if (psRR->hvContentLength == 0ULL)
		SL_ERR("'%s' invalid size (%llu)", psRR->pvArg, psRR->hvContentLength) ;
	else if (psRR->hvContentType != ctApplicationOctetStream)
		SL_ERR("'%s' invalid content (%d/%s)", psRR->pvArg, psRR->hvContentType, ctValues[psRR->hvContentType]) ;
	else if (psRR->hvConnect == coClose)
		SL_ERR("Connection closed unexpectedly") ;
	else iRV = erSUCCESS ;
	return iRV ;
}

/**
 * xHttpClientCheckFOTA() -
 * @return	1 if valid upgrade file exists
 * 			erSUCCESS if no newer upgrade file exists
 * 			erFAILURE if file not found/empty file/invalid content/connection closed
 */
int	xHttpClientCheckFOTA(http_parser * psParser, const char * pBuf, size_t xLen) {
	if (xHttpClientFileDownloadCheck(psParser) == erFAILURE)
		return erFAILURE ;
	http_rr_t * psRR = psParser->data ;
	/* BuildSeconds			: halfway(?) time of running FW
	 * hvLastModified		: creation time of available FW
	 * fotaMIN_DIF_SECONDS	: Required MIN difference (hvLastModified - BuildSeconds)
	 *						: How much later must FW be to be considered new? */
	#define	fotaMIN_DIF_SECONDS					120
	int32_t i32Diff = psRR->hvLastModified - BuildSeconds - fotaMIN_DIF_SECONDS ;
	IF_PRINT(debugTRACK && ioB1GET(ioFOTA), "'%s' found  %r vs %r  Diff=%d  FW %snewer\n", psRR->pvArg,
						psRR->hvLastModified, BuildSeconds, i32Diff, i32Diff < 0 ? "NOT " : "") ;
	if (i32Diff < 0) return erSUCCESS ;
	xRtosSetStatus(flagAPP_RESTART) ;
	return 1 ;
}

int xHttpClientPerformFOTA(http_parser * psParser, const char * pBuf, size_t xLen) {
	int iRV = xHttpClientCheckFOTA(psParser, pBuf, xLen) ;
	if (iRV != 1) return iRV;
	xRtosClearStatus(flagAPP_RESTART) ;
	fota_info_t	sFI ;
	iRV = halFOTA_Begin(&sFI) ;
	if (iRV < erSUCCESS) return erFAILURE;

	sFI.pBuf	= (void *) pBuf ;
	sFI.xLen	= xLen ;
	http_rr_t * psReq = psParser->data ;
	size_t	xLenDone = 0, xLenFull = psReq->hvContentLength ;
	IF_SYSTIMER_INIT(debugTIMING, stFOTA, stMILLIS, "halFOTA", configHTTP_RX_WAIT/10, configHTTP_RX_WAIT) ;

	while (xLen) {										// deal with all received packets
		iRV = halFOTA_Write(&sFI) ;
		if (iRV != ESP_OK) break;
		xLenDone += sFI.xLen ;
		IF_CPRINT(debugFOTA, "%d%% (%d)\r", (xLenDone * 100)/xLenFull, xLenDone) ;
		if (xLenDone == xLenFull) break;
		IF_SYSTIMER_START(debugTIMING, stFOTA) ;
		iRV = xNetReadBlocks(&psReq->sCtx, (char *) (sFI.pBuf = psReq->sUB.pBuf), psReq->sUB.Size, configHTTP_RX_WAIT) ;
		IF_SYSTIMER_STOP(debugTIMING, stFOTA) ;
		if (iRV > 0) sFI.xLen = iRV ;
		else if (psReq->sCtx.error != EAGAIN) {
			sFI.iRV = iRV ;								// save for halFOTA_End() reuse
			break ;										// no need for error reporting, already done in xNetRead()
		}
	}

	IF_SYSTIMER_SHOW_NUM(debugTIMING, stFOTA) ;
	IF_TRACK(debugFOTA, "\nWrote %u/%u from '%s/%s'\n", xLenDone, xLenFull, psReq->sCtx.pHost, psReq->pvArg) ;

	iRV = halFOTA_End(&sFI) ;
	if (iRV == erSUCCESS && sFI.iRV == ESP_OK) xRtosSetStatus(flagAPP_RESTART) ;
	return sFI.iRV ;
}

int	xHttpClientFirmwareUpgrade(void * pvPara, bool bCheck) {
	return xHttpRequest(HostInfo[sNVSvars.HostFOTA].pName, "GET /firmware/%s.bin", NULL,
			HostInfo[sNVSvars.HostFOTA].pcCert, HostInfo[sNVSvars.HostFOTA].szCert,
			bCheck == CHECK ? xHttpClientCheckFOTA : xHttpClientPerformFOTA, 0,
			httpHDR_VALUES(ctTextPlain, ctApplicationOctetStream, coKeepAlive, 0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,4), pvPara, pvPara) ;
}

/**
 * xHttpClientCheckUpgrades()
 * @		step through multiple FW upgrade options till a valid option found or until all options done.
 * @return
 */
int xHttpClientCheckUpgrades(bool bCheck) {
	/* To create a hierarchy of firmware upgrades, we need to define a descending order:
	 * #1 would be MAC based "1234567890ab.bin" hence 100% specific
	 * #2 would be "[site-token].bin"
	 * #3 would define a level to accommodate a specific client/tenant
	 * #4 would be the broadest "[device-specification-token].bin"
	 */
	int iRV = xHttpClientFirmwareUpgrade((void *) idSTA, bCheck) ;
#if 0
	if (bRtosCheckStatus(flagAPP_RESTART) == 0) {
		iRV = xHttpClientFirmwareUpgrade((void *) mqttSITE_TOKEN, bCheck) ;
	}
#endif
	if (bRtosCheckStatus(flagAPP_RESTART) == 0)
		iRV = xHttpClientFirmwareUpgrade((void *) halDEV_UUID, bCheck) ;
	xRtosClearStatus(flagAPP_UPGRADE) ;				// all options exhausted
	if (bCheck == PERFORM) SL_LOG(iRV == erFAILURE ? SL_SEV_ERROR : SL_SEV_NOTICE,
								"FW Upgrade %s", iRV == erFAILURE ? "Failed" : "Done") ;
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

int	xHttpParseGeoLoc(http_parser * psParser, const char * pcBuf, size_t xLen) {
	int	iRV = erFAILURE, NumTok ;
	const char * pKey = " Insufficient" ;
	jsmn_parser	sParser ;
	jsmntok_t *	psTokenList ;
	NumTok = xJsonParse(pcBuf, xLen, &sParser, &psTokenList) ;
	if (NumTok > 0) {									// parse Latitude
		iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "lat", &sNVSvars.GeoLocation[geoLAT], vfFXX) ;
		if (iRV >= erSUCCESS) {							// parse Longitude
			iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "lng", &sNVSvars.GeoLocation[geoLON], vfFXX) ;
			if (iRV >= erSUCCESS)						// parse accuracy
				iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "accuracy", &sNVSvars.GeoLocation[geoACC], vfFXX) ;
		}
	}
	if (iRV >= erSUCCESS && sNVSvars.GeoLocation[geoLAT] && sNVSvars.GeoLocation[geoLON]) {
		sNVSvars.fGeoLoc = 1 ;
		SystemFlag |= varFLAG_LOCATION ;
		SL_INFO("lat=%.7f  lng=%.7f  acc=%.7f",
				sNVSvars.GeoLocation[geoLAT], sNVSvars.GeoLocation[geoLON], sNVSvars.GeoLocation[geoACC]) ;
		IF_EXEC_4(debugJSON, xJsonPrintTokens, pcBuf, psTokenList, NumTok, 0) ;
	} else SL_ERR("Error parsing '%s' key", pKey) ;
	if (psTokenList) vRtosFree(psTokenList) ;
    return iRV ;
}

#ifndef keyGOOGLE
	#define	keyGOOGLE		"fakegoogle"
#endif

int	xHttpGetLocation(void) {
	if (sNVSvars.fGeoLoc) return erSUCCESS ;
	const char caQuery[] = "POST /geolocation/v1/geolocate?key="keyGOOGLE ;
	return xHttpRequest("www.googleapis.com", caQuery, "{ }\r\n",
			&CertGoogle[0], SizeGoogle, xHttpParseGeoLoc, 0,
			httpHDR_VALUES(ctApplicationJson, ctApplicationJson, 0, 0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,4), NULL) ;
}

// ##################################### TIMEZONE support ##########################################

/*
 * Google TimeZone API
 *		https://developers.google.com/maps/documentation/timezone/intro#Requests
 * Example:
 * 		https://maps.googleapis.com/maps/api/timezone/json?location=38.908133,-77.047119&timestamp=1458000000&key=YOUR_API_KEY
 */
int	xHttpParseTimeZone(http_parser * psParser, const char * pcBuf, size_t xLen) {
	jsmn_parser	sParser ;
	jsmntok_t *	psTokenList ;
	int	NumTok, iRV = erFAILURE ;
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
				if (iRV >= erSUCCESS)
					iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "timeZoneName", sNVSvars.TimeZoneName, vfSXX) ;
			}
		}
	}
	if (iRV >= erSUCCESS && sNVSvars.TimeZoneId[0] && sNVSvars.TimeZoneName[0]) {
		sNVSvars.fGeoTZ = 1 ;
		SL_INFO("%+Z(%s)", &sTSZ, sTSZ.pTZ->pcTZName) ;
		SystemFlag |= varFLAG_TIMEZONE ;
		IF_EXEC_4(debugJSON, xJsonPrintTokens, pcBuf, psTokenList, NumTok, 0) ;
	} else SL_ERR("Error parsing '%s' key", pKey) ;
	if (psTokenList) vRtosFree(psTokenList) ;
    return iRV ;
}

int	xHttpGetTimeZone(void) {
	if (sNVSvars.fGeoTZ) {
		sTZ.daylight	= sNVSvars.daylight ;
		sTZ.timezone	= sNVSvars.timezone ;
		return erSUCCESS ;
	}
	char const * caQuery = "GET /maps/api/timezone/json?location=%.7f,%.7f&timestamp=%d&key="keyGOOGLE ;
	return xHttpRequest("maps.googleapis.com", caQuery, NULL,
			CertGoogle, sizeof(CertGoogle), xHttpParseTimeZone, 0,
			httpHDR_VALUES(ctTextPlain, ctApplicationJson, 0, 0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL,
			sNVSvars.GeoLocation[geoLAT], sNVSvars.GeoLocation[geoLON]) ;
}

// ########################################## Elevation #############################################

/*
 * Google elevation API
 *		https://developers.google.com/maps/documentation/elevation/start
 * Example:
 *		https://maps.googleapis.com/maps/api/elevation/json?locations=39.7391536,-104.9847034&key=API_KEY
 */
int	xHttpParseElevation(http_parser * psParser, const char* pcBuf, size_t xLen) {
	int	iRV = erFAILURE ;
	const char * pKey = " Insufficient" ;
	jsmn_parser	sParser ;
	jsmntok_t *	psTokenList ;
	int NumTok = xJsonParse(pcBuf, xLen, &sParser, &psTokenList) ;
	if (NumTok > 0) {									// parse Elevation
		iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "elevation", &sNVSvars.GeoLocation[geoALT], vfFXX) ;
		if (iRV >= erSUCCESS)							// parse Resolution
			iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "resolution", &sNVSvars.GeoLocation[geoRES], vfFXX) ;
	}
	if (iRV >= erSUCCESS && sNVSvars.GeoLocation[geoALT]) {
		sNVSvars.fGeoAlt = 1 ;
		SystemFlag |= varFLAG_ELEVATION ;
		SL_INFO("alt=%.7f  res=%.7f", sNVSvars.GeoLocation[geoALT], sNVSvars.GeoLocation[geoRES]) ;
		IF_EXEC_4(debugJSON, xJsonPrintTokens, pcBuf, psTokenList, NumTok, 0) ;
	} else SL_ERR("Error parsing '%s' key", pKey) ;
	if (psTokenList) free(psTokenList);
    return iRV ;
}

int	xHttpGetElevation(void) {
	if (sNVSvars.fGeoAlt) return erSUCCESS ;
	const char caQuery[] = "GET /maps/api/elevation/json?locations=%.7f,%.7f&key="keyGOOGLE ;
	return xHttpRequest("maps.googleapis.com", caQuery, NULL,
			CertGoogle, sizeof(CertGoogle), xHttpParseElevation, 0,
			httpHDR_VALUES(ctTextPlain, ctApplicationJson, 0, 0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL,
			sNVSvars.GeoLocation[geoLAT], sNVSvars.GeoLocation[geoLON]) ;
}

// ############################## Combined GeoLoc dependent info ###################################

int	xHttpClientCheckGeoLoc(void) {
	int iRV = xHttpGetLocation() ;
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

int	xHttpClientPushOver(const char * pcMess, uint32_t u32Val) {
	const char caBody[] = "token="tokenPUSHOVER "&user="userPUSHOVER "&title=%U&message=%U%%40%u" ;
	return xHttpRequest("api.pushover.net", "POST /1/messages.json", caBody,
			CertGoogle, sizeof(CertGoogle), NULL, 0,
			httpHDR_VALUES(ctApplicationXwwwFormUrlencoded, ctApplicationJson, 0, 0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL,
			nameSTA, pcMess, u32Val) ;
}

// ######################################## Rules download #########################################

#if 0
#include	"sitewhere-text.h"							// for vMqttSubscribeRulesTEXT()
int xHttpClientRulesDownloadHandler(http_parser * psParser, const char * pBuf, size_t xLen) {
	if (xHttpClientFileDownloadCheck(psParser) == erFAILURE) return erFAILURE ;
	MQTTMessage	RulesMessage ;
	MessageData	RulesData ;
	RulesData.message				= &RulesMessage ;
	RulesData.message->payload		= (char *) pBuf ;
	RulesData.message->payloadlen	= xLen ;
	vMqttSubscribeRulesTEXT(&RulesData) ;
	return erSUCCESS ;
}

int	xHttpClientRulesDownload(void) {
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
int	xHttpClientIdentUpload(void * psRomID) {
	return xHttpRequest(HostInfo[sNVSvars.HostCONF].pName, "PATCH /ibuttons.dat",
			"'%m' , 'DS1990R' , 'Heavy Duty' , 'Maxim'\n",
			NULL, 0, NULL, 0, httpHDR_VALUES(ctTextPlain, 0, 0, 0),
			0, xnetDEBUG_FLAGS(0,0,0,0,0,0,0,0,0), NULL,
			psRomID) ;
}

// ################################## PUT core dump to host ########################################

int xHttpClientCoredumpUploadCB(http_rr_t * psReq) {
	int	iRV = erFAILURE ;
	/* see https://github.com/espressif/esp-idf/issues/1650 */
	size_t		xNow, xLeft, xDone = 0 ;
	IF_PRINT(debugCOREDUMP, "Coredump START upload %lld\n", psReq->hvContentLength) ;

	while (xDone < psReq->hvContentLength) {			// deal with all data to be sent
		xLeft	= psReq->hvContentLength - xDone ;
		xNow	= (xLeft > psReq->sUB.Size) ? psReq->sUB.Size : xLeft ;
		IF_PRINT(debugCOREDUMP, "Start:%u  Write:%u  Left:%u\n", xDone, xNow, xLeft) ;
		iRV = esp_partition_read((esp_partition_t *) psReq->pvArg, xDone, psReq->sUB.pBuf, xNow) ;
		if (iRV != ESP_OK) {
			SL_ERR("flash read err=0x%x (%s)", iRV, esp_err_to_name(iRV)) ;
			return -iRV ;
		}
		iRV = xNetWrite(&psReq->sCtx, (char *) psReq->sUB.pBuf, (xLeft > psReq->sUB.Size) ? psReq->sUB.Size : xLeft) ;
		if (iRV > 0) xDone += iRV ;
		else if (psReq->sCtx.error != EAGAIN) {
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

#ifdef CONFIG_ESP_COREDUMP_DATA_FORMAT_BIN
	const char caQuery[] = "PUT /coredump/%m_%X_%X_%llu.bin" ;
#elif	defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
	const char caQuery[] = "PUT /coredump/%m_%X_%X_%llu.elf" ;
#endif

int	xHttpClientCoredumpUpload(void * pvPara) {
	// for binary uploads the address and content length+type must be correct
	esp_partition_iterator_t sIter ;
	sIter = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL) ;
	IF_myASSERT(debugCOREDUMP, sIter != 0) ;

	const esp_partition_t *	psPart = esp_partition_get(sIter) ;
	IF_myASSERT(debugCOREDUMP, psPart != 0) ;

	cd_hdr_t	sCDhdr ;
	int iRV = esp_partition_read(psPart, 0, &sCDhdr, sizeof(sCDhdr)) ;
	IF_PRINT(debugCOREDUMP, "L=%d  T=%u  TCB=%u  V=%-I\n", sCDhdr.data_len, sCDhdr.tasks_num, sCDhdr.tcb_sz, sCDhdr.version) ;

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
