// x_http_client.c - Copyright 2014-24 (c) Andre M. Maree / KSS Technologies (Pty) Ltd.

#include "hal_platform.h"
#include "hal_fota.h"									// firmware download handler
#include "hal_memory.h"
#include "hal_network.h"
#include "hal_options.h"
#include "certificates.h"
#include "FreeRTOS_Support.h"
#include "parserX.h"									// parsing location & TZ requests
#include "printfx.h"
#include "syslog.h"
#include "systiming.h"
#include "x_errors_events.h"
#include "x_http_client.h"
#include "x_string_general.h"
#include "x_utilities.h"

#include "esp_ota_ops.h"								// Coredump upload

// ############################### BUILD: debug configuration options ##############################

#define	debugFLAG					0xF000
#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ###################################### BUILD : CONFIG definitions ###############################

#define configHTTP_BUFSIZE			4096
#define configHTTP_RX_WAIT			5000	// 500

#ifndef keyGOOGLE
	#define	keyGOOGLE				"fakegoogle"
#endif

#ifndef	tokenPUSHOVER
	#define	tokenPUSHOVER			"faketoken"
#endif

#ifndef	userPUSHOVER
	#define	userPUSHOVER			"fakeuser"
#endif

// ################################### Global variables ############################################

TaskHandle_t TempHandle;
extern const char * const ctValues[];
extern const char * const coValues[];

// ########################################## Location #############################################

int	xHttpParseGeoLoc(http_parser * psP, const char * pcBuf, size_t xLen) {
	jsmn_parser	sParser;
	jsmntok_t *	psTL;
	int	iRV = erFAILURE;
	const char * pKey = " Insufficient";
	int nTok = xJsonParse((char *)pcBuf, xLen, &sParser, &psTL);
	if (nTok) {											// parse latitude, longitude & accuracy
		IF_EXEC_4(debugTRACK && ioB1GET(dbHTTPreq), xJsonPrintTokens, (char *)pcBuf, psTL, nTok, 0);
		iRV = xJsonParseKeyValue((char *)pcBuf, psTL, nTok, pKey = "lat", (px_t) &sNVSvars.GeoLoc[geoLAT], cvF32);
		if (iRV >= erSUCCESS) {
			iRV = xJsonParseKeyValue((char *)pcBuf, psTL, nTok, pKey = "lng", (px_t) &sNVSvars.GeoLoc[geoLON], cvF32);
			if (iRV >= erSUCCESS)
				iRV = xJsonParseKeyValue((char *)pcBuf, psTL, nTok, pKey = "accuracy", (px_t) &sNVSvars.GeoLoc[geoACC], cvF32);
		}
	}
	if (iRV >= erSUCCESS) {
		setSYSFLAGS(vfNVSBLOB);
		SL_NOT("lat=%.7f  lng=%.7f  acc=%.7f", sNVSvars.GeoLoc[geoLAT],
				sNVSvars.GeoLoc[geoLON], sNVSvars.GeoLoc[geoACC]);
	}
	if (psTL)
		free(psTL);
    return iRV;
}

// ##################################### TIMEZONE support ##########################################

/*
 * Google TimeZone API
 *		https://developers.google.com/maps/documentation/timezone/intro#Requests
 * Example:
 * 		https://maps.googleapis.com/maps/api/timezone/json?location=38.908133,-77.047119&timestamp=1458000000&key=YOUR_API_KEY
 */
int	xHttpParseTimeZone(http_parser * psP, const char * pcBuf, size_t xLen) {
	jsmn_parser	sParser;
	jsmntok_t *	psTL;
	int	iRV = erFAILURE;
	x32_t xVal;
	const char * pKey = " Insufficient";
	int nTok = xJsonParse((char *)pcBuf, xLen, &sParser, &psTL);
	if (nTok > 0) {
		IF_EXEC_4(debugTRACK && ioB1GET(dbHTTPreq), xJsonPrintTokens, (char *)pcBuf, psTL, nTok, 0);
		iRV = xJsonParseKeyValue((char *)pcBuf, psTL, nTok, pKey = "dstOffset", (px_t) &xVal.i32, cvI32);
		if (iRV >= erSUCCESS) {
			sNVSvars.sTZ.daylight = xVal.i32;			// convert i32 -> i16 & store
			iRV = xJsonParseKeyValue((char *)pcBuf, psTL, nTok, pKey = "rawOffset", (px_t) &xVal.i32, cvI32);
			if (iRV >= erSUCCESS) {
				sNVSvars.sTZ.timezone = xVal.i32;		// store value
				iRV = xJsonParseKeyValue((char *)pcBuf, psTL, nTok, pKey = "timeZoneId", (px_t) sNVSvars.sTZ.TZid, cvSXX);
				if (iRV >= erSUCCESS)
					iRV = xJsonParseKeyValue((char *)pcBuf, psTL, nTok, pKey = "timeZoneName", (px_t) sNVSvars.sTZ.TZname, cvSXX);
			}
		}
	}
	if (iRV >= erSUCCESS) {
		setSYSFLAGS(vfNVSBLOB);
		SL_NOT("%Z(%s)", &sTSZ, sTSZ.pTZ->TZname);
	}
	if (psTL)
		free(psTL);
    return iRV;
}

// ########################################## Elevation #############################################

/*
 * Google elevation API
 *		https://developers.google.com/maps/documentation/elevation/start
 * Example:
 *		https://maps.googleapis.com/maps/api/elevation/json?locations=39.7391536,-104.9847034&key=API_KEY
 */
int	xHttpParseElevation(http_parser * psP, const char* pcBuf, size_t xLen) {
	int	iRV = erFAILURE;
	const char * pKey = " Insufficient";
	jsmn_parser	sParser;
	jsmntok_t *	psTL;
	int nTok = xJsonParse((char *)pcBuf, xLen, &sParser, &psTL);
	if (nTok > 0) {									// parse elevation & resolution
		IF_EXEC_4(debugTRACK && ioB1GET(dbHTTPreq), xJsonPrintTokens, (char *)pcBuf, psTL, nTok, 0);
		iRV = xJsonParseKeyValue((char *)pcBuf, psTL, nTok, pKey = "elevation", (px_t) &sNVSvars.GeoLoc[geoALT], cvF32);
		if (iRV >= erSUCCESS)							// parse Resolution
			iRV = xJsonParseKeyValue((char *)pcBuf, psTL, nTok, pKey = "resolution", (px_t) &sNVSvars.GeoLoc[geoRES], cvF32);
	}
	if (iRV >= erSUCCESS) {
		setSYSFLAGS(vfNVSBLOB);
		SL_NOT("alt=%.7f  res=%.7f", sNVSvars.GeoLoc[geoALT], sNVSvars.GeoLoc[geoRES]);
	}
	if (psTL) free(psTL);
    return iRV;
}

// ################################# Firmware Over The Air support #################################

/**
 * @brief	Check if a valid firmware upgrade exists
 * @return	erFAILURE if not found / empty / invalid content / connection closed
 * 			-1 if some HTTP type error
 * 			0 if no newer upgrade file exists
 * 			1 if valid upgrade file exists
 */
static int	xHttpClientCheckFOTA(http_parser * psP, const char * pBuf, size_t xLen) {
	http_rr_t * psRR = psP->data;
	int iRV = erFAILURE;
	if (psP->status_code != HTTP_STATUS_OK) {
		SL_INFO("%s (%d)", psRR->hvStatusMess, psP->status_code);
	} else if (psRR->hvContentLength == 0ULL) {
		SL_ERR("invalid size (%llu)", psRR->hvContentLength);
	} else if (psRR->hvContentType != ctApplicationOctetStream) {
		SL_ERR("invalid content (%d/%s)", psRR->hvContentType, ctValues[psRR->hvContentType]);
	} else if (psRR->hvConnect == coClose) {
		SL_ERR("Connection closed unexpectedly");
	} else {
		// BuildSeconds			: halfway(?) time of running FW
		// hvLastModified		: creation time of available FW
		// fotaMIN_DIF_SECONDS	: Required MIN difference (hvLastModified - BuildSeconds)
		//						: How much later must FW be to be considered new?
		#define	fotaMIN_DIF_SECONDS	120
		s32_t i32Diff = psRR->hvLastModified - BuildSeconds - fotaMIN_DIF_SECONDS;
		iRV = (i32Diff < 0) ? httpFW_OLD_FOUND : httpFW_NEW_FOUND;
		IF_SL_WARN(debugTRACK && ioB1GET(ioFOTA), "found %r vs %r Diff=%!r FW=%s",
				psRR->hvLastModified, BuildSeconds, i32Diff, i32Diff < 0 ? "Old" : "New");
	}
	return iRV;
}

/**
 * @brief	check if a valid download exists, if so, download and write to flash.
 * @param	psP
 * @param	pBuf
 * @param	xLen
 * @return	If error erFAILURE or less, 0 if no valid upgrade, 1 if valid upgrade
 */
static int xHttpClientPerformFOTA(http_parser * psP, const char * pBuf, size_t xLen) {
	int iRV = xHttpClientCheckFOTA(psP, pBuf, xLen);
	if (iRV < httpFW_NEW_FOUND)
		return iRV;					// <0=Error 0=OLD  
	part_xfer_t	sPX = { 0 };
	sPX.iRV = halFOTA_Begin(&sPX);
	if (sPX.iRV != erSUCCESS)
		goto exit;
	sPX.pBuf = (void *) pBuf;
	sPX.xLen = xLen;
	sPX.xDone = 0;
	http_rr_t * psReq = psP->data;
	sPX.xFull = psReq->hvContentLength;
	IF_SYSTIMER_INIT(debugTIMING, stFOTA, stMILLIS, "halFOTA", configHTTP_RX_WAIT/10, configHTTP_RX_WAIT);

	while (sPX.xLen) {									// deal with all received packets
		sPX.iRV = halFOTA_Write(&sPX);
		if (sPX.iRV != ESP_OK)							// write fail exit
			break;										// sPX.iRV is error code
		sPX.xDone += sPX.xLen;
		if (sPX.xDone == sPX.xFull)						// FOTA complete successful exit
			break;										// sPX.iRV = ESP_OK 
		IF_SYSTIMER_START(debugTIMING, stFOTA);
		sPX.iRV = xNetRecvBlocks(&psReq->sCtx, (sPX.pBuf = psReq->sUB.pBuf), psReq->sUB.Size, configHTTP_RX_WAIT);
		IF_SYSTIMER_STOP(debugTIMING, stFOTA);
		if (sPX.iRV > 0) {								// Socket Recv successful
			sPX.xLen = sPX.iRV;							// set next write length
		} else if (psReq->sCtx.error != EAGAIN) {		// Socket Recv (not EAGAIN) error exit
			break;										// sPX.iRV = Socket error code
		}
	}

	IF_SYSTIMER_SHOW_NUM(debugTIMING, stFOTA);
	iRV = halFOTA_End(&sPX);							// even if Write error, close
	if (sPX.iRV >= erSUCCESS && iRV < erSUCCESS)
		sPX.iRV = iRV;									// override with latest error code
exit:
	if (sPX.iRV >= 0)									// ESP_OK or erSUCCESS or greater...
		setSYSFLAGS(sfREBOOT);							// whole process OK, set flag
	SL_LOG((sPX.iRV < 0) ? SL_SEV_ERROR : SL_SEV_NOTICE, "%s(%d)",
			(iRV >= 0) ? "Done" : "FAIL", iRV);
	return sPX.iRV;										// return ESP_OK
}

// ###################################### Various gateways #########################################
#if 0
int	xHttpClientPushOver(const char * pcMess, u32_t u32Val) {
	return xHttpRequest("api.pushover.net", CertGGLE, SizeGGLE,
		"POST /1/messages.json", "token="tokenPUSHOVER "&user="userPUSHOVER "&title=%U&message=%U%%40%u", NULL, 0, 0,
		httpHDR_VALUES(ctApplicationXwwwFormUrlencoded, ctApplicationJson, 0, 0),
		NULL, nameSTA, pcMess, u32Val);					// No argument, 3x varargs
}

// ################################## POST IDENT info to host ######################################

/**
 * @brief	Upload single DS1990 tag info to cloud
 * @param	psRomID pointer to tag ROM ID string
 */
int	xHttpClientIdentUpload(void * psRomID) {
	return xHttpRequest(HostInfo[ioB2GET(ioHostCONF)].pName, NULL, 0, 
		"PATCH /ibuttons.dat", "{'%M' , 'DS1990R' , 'Heavy Duty' , 'Maxim' }"httpNL, NULL, 0, 0,
		httpHDR_VALUES(ctTextPlain, 0, 0, 0),
		NULL, psRomID);									// No argument, vararg
}

// ######################################### Unused API's ##########################################

// https://www.iplocation.net/
// EurekAPI.com (basic free 30day, 2017/06/06)
#define	configHTTP_HOST_EUREKAPI		"api.eurekapi.com"
#define	configHTTP_QUERY_EUREKAPI		"GET /iplocation/v1.8/locateip?key="keyEUREKA"&ip=%I&format=JSON&compact=Y"
// free geo-ip service definition
#define	configHTTP_HOST_FREE_GEO_IP		"freegeoip.net"
#define	configHTTP_FORMAT_FREE_GEO_IP	"GET /json"

// ###################################### WEATHER support ##########################################

int xHttpGetWeather(void) {
	return xHttpRequest("api.openweathermap.org", NULL, 0,
	"GET /data/2.5/forecast/?q=Johannesburg,ZA&APPID="keyOPENWEATHER, NULL, NULL, 0, 16384,
	httpHDR_VALUES(ctTextPlain, 0, 0, 0), NULL);		// No argument or varargs
}

// ################################### How's my SSL support ########################################

int	xHttpHowsMySSL(int ioHost) {
	return xHttpRequest("www.howsmyssl.com", HostInfo[ioHost].pcCert, HostInfo[ioHost].szCert,
		"GET /a/check", NULL, NULL, 0, 0,
		httpHDR_VALUES(ctTextPlain, 0, 0, 0), NULL);	// No argument or varargs
}

// ####################################### Bad SSL support #########################################

int	xHttpBadSSL(int ioHost) {
	return xHttpRequest("www.badssl.com", HostInfo[ioHost].pcCert, HostInfo[ioHost].szCert,
		"GET /dashboard", NULL, NULL, 0, 0,
		httpHDR_VALUES(ctTextPlain, 0, 0, 0), NULL);	// No argument or varargs
}
#endif
// ################################### Dynamic HTTP Task support ###################################

/**
 * @brief	Notify correct (HTTP/TNET) server task to execute an HTTP (client) request
 * @return	1 if successful (ie task running) or 0 if not
*/
bool bHttpRequestNotifyTask(u32_t AddMask) {
	if (xRtosCheckStatus(flagLX_STA) == 0)
		return 0;
	if (xRtosCheckStatus(flagCLNT_TASK)) {				// Transient HTTP client task running?
		u32_t CurMask;
		xTaskNotifyAndQuery(TempHandle, 0, eNoAction, &CurMask);
		if (CurMask) {
			xTaskNotify(TempHandle, AddMask, eSetBits);
			return 1;
		}
		// now wait until the temp task has terminated
		do { vTaskDelay(pdMS_TO_TICKS(1000)); } while (TempHandle);
		// then fall through to restart task
	}
	return (xHttpClientTaskStart((void *) AddMask) == NULL) ? 0 : 1;
}

void vTaskHttpClient(void * pvPara) {
	vTaskSetThreadLocalStoragePointer(NULL, buildFRTLSP_EVT_MASK, (void *)taskHTTP_CLNT_MASK);
	xRtosSetTaskRUN(taskHTTP_CLNT_MASK);
	bRtosTaskWaitOK(taskHTTP_CLNT_MASK, portMAX_DELAY);

	http_rr_t sRR;
	sock_sec_t sSecure;	
	http_parser sParser;
	u8_t optHost;
	u32_t Mask = (u32_t) pvPara;

	IF_SYSTIMER_INIT(debugTIMING, stHTTP, stMILLIS, "clnt", configHTTP_RX_WAIT/100, configHTTP_RX_WAIT);
	while(Mask) {
		int iRV = erSUCCESS;
		s8_t BitNum	= __builtin_ctzl(Mask);				// identify next highest priority request
		Mask &= ~(1 << BitNum);							// remove request to be handled
		part_xfer_t	sPX = { 0 };
		esp_core_dump_summary_t	sCDsummary = { 0 };
		sPX.psCDsum = &sCDsummary;
		// prepare client request
		memset(&sRR, 0, sizeof(http_rr_t));
		memset(&sSecure, 0, sizeof(sock_sec_t));
		http_parser_init(&sParser, HTTP_RESPONSE);
		psUBufCreate(&sRR.sUB, NULL, configHTTP_BUFSIZE, 0);
		sParser.data = &sRR;
		sRR.sCtx.d = ioB1GET(dbHTTPreq) ? NETX_DBG_FLAGS(0,1,0,0,0,0,0,0,0,0,0,0,0,0,3,1) :
										NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0);
		// now process the actual request...
		switch(BitNum) {
		case reqNUM_COREDUMP:
		{	iRV = esp_core_dump_get_summary(&sCDsummary);
			if (iRV == ESP_OK)
				iRV = esp_core_dump_image_get(&sPX.CDaddr, &sPX.CDsize);
			if (iRV == ESP_OK) {
				sPX.sIter = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
				IF_myASSERT(debugRESULT, sPX.sIter != 0);
				sPX.psPart = esp_partition_get(sPX.sIter);
				IF_myASSERT(debugRESULT, sPX.psPart != 0);

				optHost = ioB2GET(ioHostCONF);
				sRR.sCtx.pHost = HostInfo[optHost].pName;
				sSecure.pcCert = HostInfo[optHost].pcCert;
				sSecure.szCert = HostInfo[optHost].szCert;
				sRR.sCtx.psSec = &sSecure;
				#define httpCLNT_REQ_COREDUMP "PUT /coredump/%M_%X_%X_%llu.elf"
				uprintfx(&sRR.sUB, httpCLNT_REQ_COREDUMP, macSTA, esp_reset_reason(), DEV_FW_VER_NUM, xTimeStampAsSeconds(sTSZ.usecs));
				sRR.hdlr = halPART_Upload_CB;
				sRR.hvValues = httpHDR_VALUES(ctApplicationOctetStream, 0, 0, 0);
				sRR.hvContentLength = (u64_t) sPX.CDsize;
				sRR.pvArg = &sPX;							// Needed in upload handler				
			}
		}	break;
		case reqNUM_FW_UPG1:
		case reqNUM_FW_UPG2:
		case reqNUM_FW_CHK1:
		case reqNUM_FW_CHK2:
		{	clrSYSFLAGS(sfFW_OK);
			optHost = ioB2GET(ioHostFOTA);
			sRR.sCtx.pHost = HostInfo[optHost].pName;
			sSecure.pcCert = HostInfo[optHost].pcCert;
			sSecure.szCert = HostInfo[optHost].szCert;
			sRR.sCtx.psSec = &sSecure;
			#define httpCLNT_REQ_FIRMWARE "GET /firmware/%s.bin"
			uprintfx(&sRR.sUB, httpCLNT_REQ_FIRMWARE, BitNum == reqNUM_FW_UPG1 || BitNum==reqNUM_FW_CHK1 ? idSTA : (void *)mySTRINGIFY(buildUUID));
			sRR.sfCB.on_body = (BitNum==reqNUM_FW_UPG1 || BitNum==reqNUM_FW_UPG2) ? (http_data_cb) xHttpClientPerformFOTA : (http_data_cb) xHttpClientCheckFOTA;
			sRR.hvValues = httpHDR_VALUES(ctTextPlain, ctApplicationOctetStream, coKeepAlive, 0);
		}	break;
		case reqNUM_GEOLOC:
		{	sRR.sCtx.pHost = "www.googleapis.com";
			sSecure.pcCert = CertGGLE;
			sSecure.szCert = SizeGGLE;
			sRR.sCtx.psSec = &sSecure;
			#define httpCLNT_REQ_GEOLOC "POST /geolocation/v1/geolocate?key=%s"
			uprintfx(&sRR.sUB, httpCLNT_REQ_GEOLOC, keyGOOGLE);
			sRR.pcBody = "{ }"httpNL;
			sRR.sfCB.on_body = (http_data_cb) xHttpParseGeoLoc;
			sRR.hvValues = httpHDR_VALUES(ctApplicationJson, ctApplicationJson, 0, 0);
		}	break;
		case reqNUM_GEOTZ:
		{	sRR.sCtx.pHost = "maps.googleapis.com";
			sSecure.pcCert = CertGGLE;
			sSecure.szCert = SizeGGLE;
			sRR.sCtx.psSec = &sSecure;
			#define httpCLNT_REQ_GEOTZ "GET /maps/api/timezone/json?location=%.7f,%.7f&timestamp=%d&key=%s"
			uprintfx(&sRR.sUB, httpCLNT_REQ_GEOTZ, sNVSvars.GeoLoc[geoLAT], sNVSvars.GeoLoc[geoLON], xTimeStampAsSeconds(RunTime), keyGOOGLE);
			sRR.sfCB.on_body = (http_data_cb) xHttpParseTimeZone;
			sRR.hvValues = httpHDR_VALUES(ctTextPlain, ctApplicationJson, 0, 0);
		}	break;
		case reqNUM_GEOALT:
		{	sRR.sCtx.pHost = "maps.googleapis.com";
			sSecure.pcCert = CertGGLE;
			sSecure.szCert = SizeGGLE;
			sRR.sCtx.psSec = &sSecure;
			#define httpCLNT_REQ_GEOALT "GET /maps/api/elevation/json?locations=%.7f,%.7f&key=%s"
			uprintfx(&sRR.sUB, httpCLNT_REQ_GEOALT, sNVSvars.GeoLoc[geoLAT], sNVSvars.GeoLoc[geoLON], keyGOOGLE);
			sRR.sfCB.on_body = (http_data_cb) xHttpParseElevation;
			sRR.hvValues = httpHDR_VALUES(ctTextPlain, ctApplicationJson, 0, 0);
		}	break;
		default:
			break;
		}
		if (iRV < erSUCCESS)
			goto exit;
		IF_myASSERT(debugTRACK, sRR.hvContentType != ctUNDEFINED);
		uprintfx(&sRR.sUB, " HTTP/1.1"httpNL"Host: %s"httpNL"From: admin@irmacos.com"httpNL"User-Agent: irmacos"httpNL, sRR.sCtx.pHost);
		if (sRR.hvAccept) {
			uprintfx(&sRR.sUB, "Accept: %s"httpNL, ctValues[sRR.hvAccept]);
			sRR.hvAccept = ctUNDEFINED;
		}
		if (sRR.hvConnect)
			uprintfx(&sRR.sUB, "Connection: %s"httpNL, coValues[sRR.hvConnect]);
		uprintfx(&sRR.sUB, "Content-Type: %s"httpNL, ctValues[sRR.hvContentType]);
		if (sRR.pcBody) {								// currently handle json/xml/text/html here
			sRR.hvContentLength = (u64_t) strlen(sRR.pcBody);
			uprintfx(&sRR.sUB, "Content-Length: %llu"httpNL""httpNL, sRR.hvContentLength);
			uprintfx(&sRR.sUB, "%s", sRR.pcBody);		// add actual content
		} else if (sRR.hvContentLength) {				// some form of upload, body added by handler
				// Actual binary payload added in callback, only add a single 'httpNL' now.
				// Second 'httpNL' added at end of this function.
				// Callback will add final terminating 'httpNL'
				uprintfx(&sRR.sUB, "Content-Length: %llu"httpNL, sRR.hvContentLength);
		}
		// add the final 'httpNL' after the headers and payload, if binary payload this is 2nd pair
		uprintfx(&sRR.sUB, httpNL);
		IF_PX(debugTRACK && ioB1GET(dbHTTPreq) && sRR.sCtx.d.http, "CONTENT"strNL"%*s"strNL, xUBufGetUsed(&sRR.sUB), pcUBufTellRead(&sRR.sUB));
		// Now start the network communication portion....
		sRR.sCtx.type = SOCK_STREAM;
		sRR.sCtx.sa_in.sin_family = AF_INET;
		if (sRR.sCtx.sa_in.sin_port == 0)
			sRR.sCtx.sa_in.sin_port = htons(sRR.sCtx.psSec ? IP_PORT_HTTPS : IP_PORT_HTTP);
		sRR.sCtx.flags = SO_REUSEADDR;
		
		IF_SYSTIMER_START(debugTIMING, stHTTP);
		iRV = xNetOpen(&sRR.sCtx);
		if (iRV == erSUCCESS) {								// if socket is open
			iRV = xNetSend(&sRR.sCtx, sRR.sUB.pBuf, sRR.sUB.Used);	// write request
			if (iRV > 0 && sRR.hdlr) {						// is handler specified for [additional] body
				iRV = sRR.hdlr(&sRR);						// send body (should return same as xNetSend...)
			}
			if (iRV > 0) {									// now read the response
				iRV = xNetRecvBlocks(&sRR.sCtx, sRR.sUB.pBuf, sRR.sUB.Size, configHTTP_RX_WAIT);
				if (iRV > 0) {								// actually read something
					sRR.sUB.Used = iRV;
					iRV = xHttpCommonDoParsing(&sParser);	// return erFAILURE or some 0+ number
				} else {
					IF_PX(debugTRACK && ioB1GET(ioHTTPtrack), " nothing read ie to parse" strNL);
					iRV = erFAILURE;
				}
			} else {
				IF_PX(debugTRACK && ioB1GET(ioHTTPtrack), " nothing written (by handler) so can't expect to read" strNL);
				iRV = erFAILURE;
			}
		}
		IF_SYSTIMER_STOP(debugTIMING, stHTTP);
		xNetClose(&sRR.sCtx);								// close the socket connection if still open...
exit:
		vUBufDestroy(&sRR.sUB);								// return memory allocated
		switch(BitNum) {									// Do post processing
		case reqNUM_FW_UPG1:
		case reqNUM_FW_UPG2:
		{	if (allSYSFLAGS(sfREBOOT))						// If reboot flag set we have new FW image
				Mask &= ~reqFW_UPGRADE;						// yes, abandon possible 2nd stage
			if ((Mask & reqFW_UPGRADE) == 0) {				// If UPGRADE (1 and/or 2) completed ?
				if (allSYSFLAGS(sfREBOOT) == 0)				// yes, reboot flag set?
					setSYSFLAGS(sfFW_OK);					// no, mark FW as still valid/OK
			}
		}	break;
		case reqNUM_FW_CHK1:
		case reqNUM_FW_CHK2:
		{	if ((Mask && reqFW_CHECK) == 0) {				// both CHECK req 1&2 done
				// iRV == 1 (new FW), 0 (old FW), <0 (error)
				if (iRV < 1)
					setSYSFLAGS(sfFW_OK);
			}
		}	break;
		default:
			break;
		}
	}
	vRtosTaskDelete(TempHandle = NULL);
}

TaskHandle_t xHttpClientTaskStart(void * pvPara) {
	int iRV = xRtosTaskCreate(vTaskHttpClient, "clnt", httpCLNT_STACK_SIZE, pvPara, httpCLNT_PRIORITY, &TempHandle, tskNO_AFFINITY);
	return (iRV == pdPASS) ? TempHandle : (TempHandle = NULL);
}

/*	TODO:
	Investigate counter for enums remove manual value checking

	http://wolkykim.github.io/libasyncd/
	https://www.gnu.org/software/libmicrohttpd/

*/