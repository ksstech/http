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

// ############################### BUILD: debug configuration options ##############################

#define	debugFLAG					0xF000
#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ###################################### BUILD : CONFIG definitions ###############################

#define httpBUFSIZE_NONE			0
#define httpDATASIZE_NONE			0

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

extern TaskHandle_t TnetHandle;
extern const char * const ctValues[];
extern const char * const coValues[];

// ################################### Common HTTP API functions ###################################

/**
 * @brief	Notify correct (HTTP/TNET) server task to execute an HTTP (client) request 
*/
void vHttpRequestNotifyTask(u32_t ulValue) {
#if (includeHTTP_TASK > 0)
	xTaskNotify(HttpHandle, ulValue, eSetBits);
#elif (includeTNET_TASK > 0)
	xTaskNotify(TnetHandle, ulValue, eSetBits);
#else
	#error "No task configured to handle HTTP requests"
#endif
}

/**
 * @brief	
 * @return	
*/
int vHttpRequestNotifyHandler(void) {
	u32_t fRqst = 0, fDone = 0;
	int iRV = erSUCCESS;
	if (xTaskNotifyWait(0, 0, &fRqst, pdMS_TO_TICKS(0)) == pdTRUE) {
		SL_INFO("Received Notify x%X", fRqst);
		if (fRqst & reqCOREDUMP) {
			iRV = xHttpCoredumpUpload();
			fDone |= reqCOREDUMP;
		}
		if (fRqst & reqFW_UPGRADE) {
			xRtosClearTaskRUN(taskGUI_MASK);			// Stop GUI task
			iRV = xHttpClientCheckUpgrades(PERFORM);
			xRtosSetTaskRUN(taskGUI_MASK);				// Start GUI task
			fDone |= reqFW_UPGRADE;
		}
		if (fRqst & reqFW_CHECK) {
			xHttpClientCheckUpgrades(CHECK);
			fDone |= reqFW_CHECK;
		}
		if (allSYSFLAGS(sfREBOOT) == 0) {				// reboot NOT requested
			if (fRqst & reqGEOLOC) {
				iRV = xHttpGetLocation();
				if (iRV > erFAILURE) fDone |= reqGEOLOC;
			}
			if (fRqst & reqGEOTZ) {
				iRV = xHttpGetTimeZone();
				if (iRV > erFAILURE) fDone |= reqGEOTZ;
			}
			if (fRqst & reqGEOALT) {
				iRV = xHttpGetElevation();
				if (iRV > erFAILURE) fDone |= reqGEOALT;
			}
		} else if (fRqst & (reqGEOLOC|reqGEOTZ|reqGEOALT)) {	// REBOOT is requested
			fDone |= (reqGEOLOC|reqGEOTZ|reqGEOALT);	// discard whatever is requested
			SL_INFO("GeoXXX requests discarded, restart...");
		}
		if (fDone) {
			SL_INFO("fRqst=x%X  fDone=x%X", fRqst, fDone);
			ulTaskNotifyValueClear(NULL, fDone);
		}
	}
	return iRV;
}

int	xHttpBuildHeader(http_parser * psParser) {
	http_rr_t * psRR = psParser->data;
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psParser) && halCONFIG_inSRAM(psRR) && halCONFIG_inSRAM(psRR->sUB.pBuf));

	vuprintfx(&psRR->sUB, psRR->pcQuery, psRR->VaList);
	uprintfx(&psRR->sUB, " HTTP/1.1\r\nHost: %s\r\nFrom: admin@irmacos.com\r\nUser-Agent: irmacos\r\n", psRR->sCtx.pHost);
	if (psRR->hvAccept) {
		uprintfx(&psRR->sUB, "Accept: %s\r\n", ctValues[psRR->hvAccept]);
		psRR->hvAccept	= ctUNDEFINED;
	}
	if (psRR->hvConnect)
		uprintfx(&psRR->sUB, "Connection: %s\r\n", coValues[psRR->hvConnect]);
	// from here on items common to requests and responses...
	if (psRR->pcBody) {
		if (psRR->hvContentType) {
			uprintfx(&psRR->sUB, "Content-Type: %s\r\n", ctValues[psRR->hvContentType]);
			if (psRR->hvContentType == ctApplicationOctetStream) {
				IF_myASSERT(debugTRACK, INRANGE(1, psRR->hvContentLength, 2*MEGA));
				/* Since the actual binary payload will only be added in the callback
				 * we will only add a single CRLF
				 * pair here, the second added at the end of this function.
				 * The callback will be responsible for adding the final terminating strCRLF */
				uprintfx(&psRR->sUB, "Content-Length: %d\r\n", psRR->hvContentLength);
				// no actual binary content added, done later...
			} else {									// currently handle json/xml/text/html here
				psRR->hvContentLength = vsnprintfx(NULL, xpfMAXLEN_MAXVAL, psRR->pcBody, psRR->VaList);	// determine body length
				uprintfx(&psRR->sUB, "Content-Length: %d\r\n\r\n", psRR->hvContentLength);
				vuprintfx(&psRR->sUB, psRR->pcBody, psRR->VaList);// add actual content
			}
		} else {
			SL_ERR(debugAPPL_PLACE);
		}
	}
	// add the final CR after the headers and payload, if binary payload this is 2nd strCRLF pair
	uprintfx(&psRR->sUB, strCRLF);
	IF_PX(debugTRACK && ioB1GET(ioHTTPtrack) && psRR->sCtx.d.http, "Content:\r\n%*s\r\n", xUBufGetUsed(&psRR->sUB), pcUBufTellRead(&psRR->sUB));
	return psRR->sUB.Used;
}

/**
 * @brief	Build HTTP request packet and initiates connect, parse, respond & disconnect activities
 * @param	pHost - pointer to hostname to connect to
 * @param	pcCert - pointer to TLS certificate
 * @param	szCert - size of TLS certificate
 * @param	pQuery - HTTP query string, can contain printf formatting string
 * @param	pvBody - POST request ??
 * 			pvBody - GET request (NULL ??)
 *			pvBody - PUT request handler ??
 * @param	OnBodyCB - Callback to handle the response body
 * @param	DataSize - size of data to be transmitted
 * @param	BufSize - size of Tx/RX buffer to be allocated, if NULL use default
 * @param	hvValues - header values
 * @param	pvArg - POST request
 * 			pvArg - GET request (NULL ??)
 * 			pvArg - PUT request (parameter for handler)
 * @param	varArgs length list of arguments for use with pQuery
 * @return	erFAILURE or result of xHttpCommonDoParsing() being 0 or more
 */
int	xHttpRequest(pcc_t pHost, pcc_t pcCert, size_t szCert, const char *pQuery, const void * xUnion,
		void * OnBodyCB, u32_t DataSize, u16_t BufSize, u32_t hvValues, void * pvArg, ...) {
	http_rr_t sRR = { 0 };
	sock_sec_t sSecure = { 0 };		// LEAVE here else pcCert/szCert gets screwed
	sRR.sCtx.pHost = pHost;
	if (pcCert) {
		sRR.sCtx.psSec = &sSecure;
		sSecure.pcCert = pcCert;
		sSecure.szCert = szCert;
	}
	sRR.pcQuery = pQuery;
	sRR.xUnion = xUnion;
	sRR.sfCB.on_body = (http_data_cb) OnBodyCB;
	sRR.hvContentLength	= (u64_t) DataSize;
	psUBufCreate(&sRR.sUB, NULL, BufSize ? BufSize : configHTTP_BUFSIZE, 0);	// setup ubuf_t structure
	sRR.hvValues = hvValues;
	sRR.pvArg = pvArg;
	// Default xNet debug flags
	sRR.sCtx.d = ioB1GET(dbgHTTPreq) ? NETX_DBG_FLAGS(0,1,0,0,0,0,0,0,0,0,0,0,0,0,3,1) :
										NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0);

	http_parser sParser;
	http_parser_init(&sParser, HTTP_RESPONSE);			// clear all parser fields/values
	sParser.data = &sRR;

	IF_PX(debugTRACK && ioB1GET(ioHTTPtrack), "H='%s'  Q='%s'  cb=%p  hv=0x%08X  B=", sRR.sCtx.pHost, sRR.pcQuery, sRR.sfCB.on_body, sRR.hvValues);
	IF_PX(debugTRACK && ioB1GET(ioHTTPtrack), sRR.hvContentType == ctApplicationOctetStream ? "%p\r\n" : "%s\r\n", sRR.xUnion);
	IF_myASSERT(debugTRACK, sRR.hvContentType != ctUNDEFINED);
	IF_SYSTIMER_INIT(debugTIMING, stHTTP, stMILLIS, "HTTPclnt", configHTTP_RX_WAIT/100, configHTTP_RX_WAIT);
	IF_SYSTIMER_START(debugTIMING, stHTTP);

	va_list vArgs;
	va_start(vArgs, pvArg);
	sRR.VaList = vArgs;
	int xLen = xHttpBuildHeader(&sParser);
	va_end(vArgs);

	sRR.sCtx.type = SOCK_STREAM;
	sRR.sCtx.sa_in.sin_family = AF_INET;
	if (sRR.sCtx.sa_in.sin_port == 0)
		sRR.sCtx.sa_in.sin_port = htons(sRR.sCtx.psSec ? IP_PORT_HTTPS : IP_PORT_HTTP);
	sRR.sCtx.flags = SO_REUSEADDR;
	int iRV = xNetOpen(&sRR.sCtx);
	if (iRV == erSUCCESS) {								// if socket=open, write request
		iRV = xNetSend(&sRR.sCtx, sRR.sUB.pBuf, xLen);
		if (iRV > 0) {									// successfully written some (or all)
			if (sRR.hvContentType == ctApplicationOctetStream)
				iRV = sRR.hdlr_req(&sRR);				// should return same as xNetSendX()
			if (iRV > 0) {								// now do the actual read
				iRV = xNetRecvBlocks(&sRR.sCtx, sRR.sUB.pBuf, sRR.sUB.Size, configHTTP_RX_WAIT);
				if (iRV > 0) {							// actually read something
					sRR.sUB.Used = iRV;
					iRV = xHttpCommonDoParsing(&sParser);	// return erFAILURE or some 0+ number
				} else {
					IF_PX(debugTRACK && ioB1GET(ioHTTPtrack), " nothing read ie to parse\r\n");
					iRV = erFAILURE;
				}
			} else {
				IF_PX(debugTRACK && ioB1GET(ioHTTPtrack), " nothing written (by handler) so can't expect to read\r\n");
				iRV = erFAILURE;
			}
		}
	}
	xNetClose(&sRR.sCtx);								// close the socket connection if still open...
	vUBufDestroy(&sRR.sUB);								// return memory allocated
	IF_SYSTIMER_STOP(debugTIMING, stHTTP);
	return iRV;
}

// ########################################## Location #############################################

int	xHttpParseGeoLoc(http_parser * psParser, const char * pcBuf, size_t xLen) {
	jsmn_parser	sParser;
	jsmntok_t *	psTL;
	int	iRV = erFAILURE;
	const char * pKey = " Insufficient";
	int nTok = xJsonParse((char *)pcBuf, xLen, &sParser, &psTL);
	if (nTok) {											// parse latitude, longitude & accuracy
		IF_EXEC_4(debugTRACK && ioB1GET(dbgHTTPreq), xJsonPrintTokens, (char *)pcBuf, psTL, nTok, 0);
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
	if (psTL) free(psTL);
    return iRV;
}

int	xHttpGetLocation(void) {
	return xHttpRequest("www.googleapis.com", CertGGLE, SizeGGLE,
		"POST /geolocation/v1/geolocate?key="keyGOOGLE, "{ }\r\n",
		xHttpParseGeoLoc, httpDATASIZE_NONE, httpBUFSIZE_NONE,
		httpHDR_VALUES(ctApplicationJson, ctApplicationJson, 0, 0),
		NULL);	// no parameters
}

// ##################################### TIMEZONE support ##########################################

/*
 * Google TimeZone API
 *		https://developers.google.com/maps/documentation/timezone/intro#Requests
 * Example:
 * 		https://maps.googleapis.com/maps/api/timezone/json?location=38.908133,-77.047119&timestamp=1458000000&key=YOUR_API_KEY
 */
int	xHttpParseTimeZone(http_parser * psParser, const char * pcBuf, size_t xLen) {
	jsmn_parser	sParser;
	jsmntok_t *	psTL;
	int	iRV = erFAILURE;
	x32_t xVal;
	const char * pKey = " Insufficient";
	int nTok = xJsonParse((char *)pcBuf, xLen, &sParser, &psTL);
	if (nTok > 0) {
		IF_EXEC_4(debugTRACK && ioB1GET(dbgHTTPreq), xJsonPrintTokens, (char *)pcBuf, psTL, nTok, 0);
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
	if (psTL) free(psTL);
    return iRV;
}

int	xHttpGetTimeZone(void) {
	return xHttpRequest("maps.googleapis.com", CertGGLE, SizeGGLE,
		"GET /maps/api/timezone/json?location=%.7f,%.7f&timestamp=%d&key="keyGOOGLE, NULL,
		xHttpParseTimeZone, httpDATASIZE_NONE, httpBUFSIZE_NONE,
		httpHDR_VALUES(ctTextPlain, ctApplicationJson, 0, 0),
		NULL, sNVSvars.GeoLoc[geoLAT], sNVSvars.GeoLoc[geoLON], xTimeStampAsSeconds(RunTime));
}

// ########################################## Elevation #############################################

/*
 * Google elevation API
 *		https://developers.google.com/maps/documentation/elevation/start
 * Example:
 *		https://maps.googleapis.com/maps/api/elevation/json?locations=39.7391536,-104.9847034&key=API_KEY
 */
int	xHttpParseElevation(http_parser * psParser, const char* pcBuf, size_t xLen) {
	int	iRV = erFAILURE;
	const char * pKey = " Insufficient";
	jsmn_parser	sParser;
	jsmntok_t *	psTL;
	int nTok = xJsonParse((char *)pcBuf, xLen, &sParser, &psTL);
	if (nTok > 0) {									// parse elevation & resolution
		IF_EXEC_4(debugTRACK && ioB1GET(dbgHTTPreq), xJsonPrintTokens, (char *)pcBuf, psTL, nTok, 0);
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

int	xHttpGetElevation(void) {
	return xHttpRequest("maps.googleapis.com", CertGGLE, SizeGGLE,
		"GET /maps/api/elevation/json?locations=%.7f,%.7f&key="keyGOOGLE, NULL,
		xHttpParseElevation, httpDATASIZE_NONE, httpBUFSIZE_NONE,
		httpHDR_VALUES(ctTextPlain, ctApplicationJson, 0, 0),
		NULL, sNVSvars.GeoLoc[geoLAT], sNVSvars.GeoLoc[geoLON]);	// Lat+Lon as parameter
}

// ################################# Firmware Over The Air support #################################

/**
 * @brief	Check if a valid firmware upgrade exists
 * @return	erFAILURE if not found / empty / invalid content / connection closed
 * 			0 if no newer upgrade file exists
 * 			1 if valid upgrade file exists
 */
static int	xHttpClientCheckFOTA(http_parser * psParser, const char * pBuf, size_t xLen) {
	http_rr_t * psRR = psParser->data;
	int iRV = erFAILURE;
	if (psParser->status_code != HTTP_STATUS_OK) {
		IF_SL_INFO(debugTRACK && ioB1GET(dbgHTTPreq), "'%s' Error=%d", psRR->pvArg, psParser->status_code);

	} else if (psRR->hvContentLength == 0ULL) {
		SL_ERR("invalid size (%llu)", psRR->hvContentLength);

	} else if (psRR->hvContentType != ctApplicationOctetStream) {
		SL_ERR("invalid content (%d/%s)", psRR->hvContentType, ctValues[psRR->hvContentType]);

	} else if (psRR->hvConnect == coClose) {
		SL_ERR("Connection closed unexpectedly");

	} else {
		/* BuildSeconds			: halfway(?) time of running FW
		 * hvLastModified		: creation time of available FW
		 * fotaMIN_DIF_SECONDS	: Required MIN difference (hvLastModified - BuildSeconds)
		 *						: How much later must FW be to be considered new? */
		#define	fotaMIN_DIF_SECONDS	120
		s32_t i32Diff = psRR->hvLastModified - BuildSeconds - fotaMIN_DIF_SECONDS;
		iRV = (i32Diff < 0) ? erSUCCESS : 1;
		IF_SL_NOT(debugTRACK && ioB1GET(dbgHTTPreq), "found  %R vs %R  Diff=%ld  FW %s",
				xTimeMakeTimeStamp(psRR->hvLastModified, 0),
				xTimeMakeTimeStamp(BuildSeconds, 0), i32Diff, i32Diff < 0 ? "old" : "new");
	}
	return iRV;
}

/**
 * @brief	check if a valid download exists, if so, download and write to flash.
 * @param	psParser
 * @param	pBuf
 * @param	xLen
 * @return	If error erFAILURE or less, 0 if no valid upgrade, 1 if valid upgrade
 */
static int xHttpClientPerformFOTA(http_parser * psParser, const char * pBuf, size_t xLen) {
	int iRV = xHttpClientCheckFOTA(psParser, pBuf, xLen);
	if (iRV <= 0)
		return iRV;					// 0 = NotNew  -1 = Error
	part_xfer_t	sFI;
	iRV = halFOTA_Begin(&sFI);
	if (iRV != erSUCCESS)
		return iRV;
	sFI.pBuf = (void *) pBuf;
	sFI.xLen = xLen;
	sFI.xDone = 0;
	http_rr_t * psReq = psParser->data;
	sFI.xFull = psReq->hvContentLength;
	IF_SYSTIMER_INIT(debugTIMING, stFOTA, stMILLIS, "halFOTA", configHTTP_RX_WAIT/10, configHTTP_RX_WAIT);

	while (xLen) {										// deal with all received packets
		iRV = halFOTA_Write(&sFI);
		if (iRV != ESP_OK)
			break;
		sFI.xDone += sFI.xLen;
		if (sFI.xDone == sFI.xFull)
			break;
		IF_SYSTIMER_START(debugTIMING, stFOTA);
		iRV = xNetRecvBlocks(&psReq->sCtx, (sFI.pBuf = psReq->sUB.pBuf), psReq->sUB.Size, configHTTP_RX_WAIT);
		IF_SYSTIMER_STOP(debugTIMING, stFOTA);
		if (iRV > 0) {
			sFI.xLen = iRV;
		} else if (psReq->sCtx.error != EAGAIN) {
			sFI.iRV = iRV;								// save for halFOTA_End() reuse
			break;										// no need for error reporting, already done in xNetRecv()
		}
	}

	IF_SYSTIMER_SHOW_NUM(debugTIMING, stFOTA);
	int iRV1 = halFOTA_End(&sFI);						// even if Write error, close
	if (iRV < erSUCCESS)
		return iRV;					// Write error, return
	if (iRV1 < erSUCCESS)
		return iRV1;					// End error, return
	if (iRV == erSUCCESS && sFI.iRV == ESP_OK)
		setSYSFLAGS(sfREBOOT);
	return sFI.iRV;
}

/**
 * @brief	Initiate FW upgrade check or request
 * @return	If error erFAILURE or less, 0 if no valid upgrade or result from xHttpClientPerformFOTA()
*/
static int xHttpClientFirmwareUpgrade(void * pvFileName, bool bCheck) {
	u8_t optHost = ioB2GET(ioHostFOTA);
	return xHttpRequest(HostInfo[optHost].pName, HostInfo[optHost].pcCert, HostInfo[optHost].szCert,
		"GET /firmware/%s.bin", NULL,
		bCheck == CHECK ? xHttpClientCheckFOTA : xHttpClientPerformFOTA, httpDATASIZE_NONE, httpBUFSIZE_NONE,
		httpHDR_VALUES(ctTextPlain, ctApplicationOctetStream, coKeepAlive, 0),
		NULL, pvFileName);		// firmware filename as parameter into query
}

/**
 * @brief	step through multiple FW upgrade options till a valid option found or until all options done.
 * @return	
 */
int xHttpClientCheckUpgrades(bool bCheck) {
	clrSYSFLAGS(sfFW_OK);
	/* To create a hierarchy of firmware upgrades, we need to define a descending order:
	 * #1 MAC address: "1234567890ab.bin"
	 * #2 hardware platform: "device-specification-token.bin"
	 * #3 to be defined
	 */
	int iRV = xHttpClientFirmwareUpgrade((void *) idSTA, bCheck);
	if (allSYSFLAGS(sfREBOOT) == 0)
		iRV = xHttpClientFirmwareUpgrade((void *) mySTRINGIFY(buildUUID), bCheck);
	if (bCheck == PERFORM)
		SL_LOG(iRV < erSUCCESS ? SL_SEV_ERROR : SL_SEV_NOTICE, "FWupg %s", iRV < erSUCCESS ? "FAIL" : "Done");
	if (allSYSFLAGS(sfREBOOT) == 0) 
		setSYSFLAGS(sfFW_OK);
	return iRV;
}

// ################################## PUT core dump to host ########################################

/**
 * @brief 
 * @return	result from esp_partition_read() or xHttpRequest()
 */
int xHttpCoredumpUpload(void) {
	esp_core_dump_summary_t	sCDsummary = { 0 };
	size_t CDaddr, CDsize;
	int iRV = esp_core_dump_get_summary(&sCDsummary);
	if (iRV == ESP_OK) {
		iRV = esp_core_dump_image_get(&CDaddr, &CDsize);
		if (iRV == ESP_OK) {
			SL_WARN("Ver=%-I  Task='%s'  Addr=%p  Size=%lu", sCDsummary.core_dump_version, sCDsummary.exc_task, CDaddr, CDsize);
		}
	}
	if (iRV >= ESP_OK) {
		esp_partition_iterator_t sIter;
		sIter = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
		IF_myASSERT(debugRESULT, sIter != 0);

		const esp_partition_t *	psPart = esp_partition_get(sIter);
		IF_myASSERT(debugRESULT, psPart != 0);

		u8_t optHost = ioB2GET(ioHostCONF);
		iRV = xHttpRequest(HostInfo[optHost].pName, HostInfo[optHost].pcCert, HostInfo[optHost].szCert,
			"PUT /coredump/%M_%X_%X_%llu.elf", halPART_Upload_CB,
			NULL, CDsize, httpBUFSIZE_NONE,
			httpHDR_VALUES(ctApplicationOctetStream, 0, 0, 0),
			(void *) psPart, macSTA, esp_reset_reason(), DEV_FW_VER_NUM, xTimeStampAsSeconds(sTSZ.usecs));
		esp_partition_iterator_release(sIter);
	}
	return iRV;
}

// ###################################### Various gateways #########################################

int	xHttpClientPushOver(const char * pcMess, u32_t u32Val) {
	return xHttpRequest("api.pushover.net", CertGGLE, SizeGGLE,
			"POST /1/messages.json", "token="tokenPUSHOVER "&user="userPUSHOVER "&title=%U&message=%U%%40%u",
			NULL, httpDATASIZE_NONE, httpBUFSIZE_NONE,
			httpHDR_VALUES(ctApplicationXwwwFormUrlencoded, ctApplicationJson, 0, 0),
			NULL, nameSTA, pcMess, u32Val);	// No argument, 3x varargs
}

// ################################## POST IDENT info to host ######################################

/**
 * @brief	Upload single DS1990 tag info to cloud
 * @param	psRomID pointer to tag ROM ID string
 */
int	xHttpClientIdentUpload(void * psRomID) {
	return xHttpRequest(HostInfo[ioB2GET(ioHostCONF)].pName, NULL, 0, 
			"PATCH /ibuttons.dat", "{'%M' , 'DS1990R' , 'Heavy Duty' , 'Maxim' }\r\n",
			NULL, httpDATASIZE_NONE, httpBUFSIZE_NONE,
			httpHDR_VALUES(ctTextPlain, 0, 0, 0),
			NULL, psRomID);			// No argument, vararg
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
			"GET /data/2.5/forecast/?q=Johannesburg,ZA&APPID="keyOPENWEATHER, NULL,
			NULL, httpDATASIZE_NONE, 16384,
			httpHDR_VALUES(ctTextPlain, 0, 0, 0),
			NULL);					// No argument or varargs
}

// ################################### How's my SSL support ########################################

int	xHttpHowsMySSL(int ioHost) {
	return xHttpRequest("www.howsmyssl.com", HostInfo[ioHost].pcCert, HostInfo[ioHost].szCert,
			"GET /a/check", NULL,
			NULL, httpDATASIZE_NONE, httpBUFSIZE_NONE,
			httpHDR_VALUES(ctTextPlain, 0, 0, 0),
			NULL);					// No argument or varargs
}

// ####################################### Bad SSL support #########################################

int	xHttpBadSSL(int ioHost) {
	return xHttpRequest("www.badssl.com", HostInfo[ioHost].pcCert, HostInfo[ioHost].szCert,
			"GET /dashboard", NULL,
			NULL, httpDATASIZE_NONE, httpBUFSIZE_NONE,
			httpHDR_VALUES(ctTextPlain, 0, 0, 0),
			NULL);					// No argument or varargs
}
