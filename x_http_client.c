// x_http_client.c - Copyright 2014-25 (c) Andre M. Maree / KSS Technologies (Pty) Ltd.

#include "hal_platform.h"
#include "hal_flash.h"									// firmware download handler
#include "hal_memory.h"
#include "hal_network.h"
#include "hal_options.h"
#include "certificates.h"
#include "FreeRTOS_Support.h"
#include "parserX.h"									// parsing location & TZ requests
#include "printfx.h"
#include "syslog.h"
#include "systiming.h"
#include "errors_events.h"
#include "x_http_client.h"
#include "string_general.h"
#include "utilitiesX.h"

#include "esp_ota_ops.h"								// Coredump upload

// ############################### BUILD: debug configuration options ##############################

#define	debugFLAG					0xF000
#define debugDETAIL					(debugFLAG & 0x0001)
#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ###################################### BUILD : CONFIG definitions ###############################

#define configHTTP_BUFSIZE			4096
#define configHTTP_RX_WAIT			200	// 500 1000 5000

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

static TaskHandle_t TempHandle;
extern const char * const ctValues[];
extern const char * const coValues[];

// ######################################### Constants ############################################

#define buildHTTP_PARSE_ENTRY(name,addr,form) { name, { addr }, form },

const ph_entries_t saEntryGeoCode = { 1, {
	buildHTTP_PARSE_ENTRY("short_name", &sNVSvars.GeoCode[0], cvSXX)
} };

const ph_entries_t saEntryGeoAlt = { 2, {
	buildHTTP_PARSE_ENTRY("elevation", &sNVSvars.GeoLoc[geoALT], cvF32)
	buildHTTP_PARSE_ENTRY("resolution", &sNVSvars.GeoLoc[geoRES], cvF32)
} };

const ph_entries_t saEntryGeoLoc = { 3, {
	buildHTTP_PARSE_ENTRY("lat", &sNVSvars.GeoLoc[geoLAT], cvF32)
	buildHTTP_PARSE_ENTRY("lng", &sNVSvars.GeoLoc[geoLON], cvF32)
	buildHTTP_PARSE_ENTRY("accuracy", &sNVSvars.GeoLoc[geoACC], cvF32)
} };

const ph_entries_t saEntryGeoTZ = { 4, {
	buildHTTP_PARSE_ENTRY("dstOffset", &sNVSvars.sTZ.daylight, cvI16)
	buildHTTP_PARSE_ENTRY("rawOffset", &sNVSvars.sTZ.timezone, cvI32)
	buildHTTP_PARSE_ENTRY("timeZoneId", sNVSvars.sTZ.TZid, cvSXX)
	buildHTTP_PARSE_ENTRY("timeZoneName", sNVSvars.sTZ.TZname, cvSXX)
} };

// ########################################## Generic ##############################################

/**
 * 
 */
int	xHttpParseGeneric(http_parser * psP, char * pcBuf, size_t xLen) {
	parse_hdlr_t sPH = { 0 };
	sPH.pcBuf = pcBuf;
	sPH.szBuf = xLen;
	sPH.pvArg = psP;
	int iRV = xJsonParse(&sPH);
	if (sPH.NumTok > 0) {
		http_rr_t * psRR = psP->data;
		ph_entries_t * psPHEx = psRR->pvArg;
		ph_entry_t * psPHE = &psPHEx->Entry[0];			// retrieve pointer to entries to be parsed
		u8_t Count = psPHEx->Count;
		int Done = 0;
		while (Count) {
			Done += xJsonParseEntry(&sPH, psPHE);
			++psPHE;									// step to next KEY:VALUE pair to be parsed
			--Count;									// adjust remaining pair count
		}
		if (Done) halVarsUpdateBlobs(vfNVSBLOB);
	}
	if (sPH.psT0) free(sPH.psT0);
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
static int	xHttpClientCheckNewer(http_parser * psP, const char * pBuf, size_t xLen) {
	http_rr_t * psRR = psP->data;
	psRR->onBodyRet = erFAILURE;
	if (psP->status_code != HTTP_STATUS_OK) {
		SL_INFO("%s (%d)", psRR->hvStatusMess, psP->status_code);
	} else if (psRR->hvContentLength == 0ULL) {
		SL_ERR("Invalid size (%llu)", psRR->hvContentLength);
	} else if (psRR->hvContentType != ctApplicationOctetStream) {
		SL_ERR("Invalid content (%d/%s)", psRR->hvContentType, ctValues[psRR->hvContentType]);
	} else if (psRR->hvConnect == coClose) {
		SL_ERR("Connection closed unexpectedly");
	} else {
		part_xfer_t	* psPX = psRR->pvArg;
		s32_t i32Diff = psRR->hvLastModified - psPX->tLow - psPX->tDiff;
		psRR->onBodyRet = (i32Diff < 0) ? httpFW_OLD_FOUND : httpFW_NEW_FOUND;
		SL_INFO("found %r vs %r Diff=%!r '%s'", psRR->hvLastModified, psPX->tLow, i32Diff, i32Diff < 0 ? "Old" : "NEW");
	}
	return psRR->onBodyRet;
}

/**
 * @brief	check if a valid download exists, if so, download and write to flash.
 * @param	psP
 * @param	pBuf
 * @param	xLen
 * @return	If error erFAILURE or less, 0 if no valid upgrade, 1 if valid upgrade
 */
static int xHttpClientDownload(http_parser * psP, const char * pBuf, size_t xLen) {
	xHttpClientCheckNewer(psP, pBuf, xLen);
	http_rr_t * psRR = psP->data;
	if (psRR->onBodyRet < httpFW_NEW_FOUND)			return psRR->onBodyRet;		// <0=Error 0=OLD
	#if (appOPTIONS == 1)
		u8_t Option = ioB1GET(ioFOTA);
	#else
		#warning "Options support required for proper functioning!!!"
		u8_t Option = 0;
	#endif
	part_xfer_t	* psPX = psRR->pvArg;
	psPX->pBuf = (void *) pBuf;
	psPX->xLen = xLen;
	psPX->xDone = 0;
	psPX->xFull = psRR->hvContentLength;

	IF_SYSTIMER_INIT(debugTIMING, stFOTA, stMILLIS, "FOTA", configHTTP_RX_WAIT/10, configHTTP_RX_WAIT);
	IF_myASSERT(debugPARAM, psPX->psHdlr && psPX->psHdlr->start && psPX->psHdlr->body && psPX->psHdlr->stop);
	psPX->psHdlr->start(psPX);
	if (psPX->iRV < erSUCCESS)						goto exit;
	IF_PX(debugTRACK && Option, "Started %ld/%lu" strNL, psPX->xLen, psPX->xFull);
	while (psPX->xLen) {								// deal with all received packets
		psPX->psHdlr->body(psPX);						// call OTA/BL/??? write handler
		if (psPX->iRV < erSUCCESS)						// write fail
			break;										// psPX->iRV is error code
		psPX->xDone += psPX->xLen;
		IF_PX(debugTRACK && Option, "%d%%  \r", (psPX->xDone * 100) / psPX->xFull);
		if (psPX->xDone == psPX->xFull)					// All done?
			break;										// psPX->iRV = ESP_OK
		IF_SYSTIMER_START(debugTIMING, stFOTA);
		psPX->iRV = xNetRecv(&psRR->sCtx, (psPX->pBuf = psRR->sUB.pBuf), psRR->sUB.Size);
		IF_SYSTIMER_STOP(debugTIMING, stFOTA);
		if (psPX->iRV >= 0) {							// Socket Recv successful
			psPX->xLen = psPX->iRV;						// set next write length
		} else if (psRR->sCtx.error != EAGAIN) {		// Socket Recv (not EAGAIN) error
			break;										// psPX->iRV = Socket error code
		}
	}
	IF_PX(debugTRACK && Option, strNL "Stopped (%ld)" strNL, psPX->xFull - psPX->xDone);
	IF_SYSTIMER_SHOW_NUM(debugTIMING, stFOTA);
	psPX->psHdlr->stop(psPX);							// even if Write error, close
exit:
	SL_LOG((psPX->iRV < 0) ? SL_SEV_ERROR : SL_SEV_NOTICE, "%s (%s/%d)", (psPX->iRV < 0) ? "FAIL" : "Done", esp_err_to_name(psPX->iRV),psPX->iRV);
	return psPX->iRV;									// return ESP_OK
}

// ################################### Dynamic HTTP Task support ###################################

/**
 * @brief	Notify correct (HTTP/TNET) server task to execute an HTTP (client) request
 * @return	1 if successful (ie task running) or 0 if not
*/
bool bHttpRequestNotifyTask(u32_t AddMask) {
	if (xRtosCheckStat0(flagLX_STA) == 0)			return 0;
	if (xRtosCheckStat0(flagCLNT_TASK)) {				// Transient HTTP client task running?
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

static void vTaskHttpClient(void * pvPara) {
	u32_t Mask = (u32_t) pvPara;
	halEventUpdateRunTasks(0, 1);
	halEventWaitTasksOK(0, portMAX_DELAY);
	IF_SYSTIMER_INIT(debugTIMING, stHTTP, stMILLIS, "clnt", configHTTP_RX_WAIT/10, configHTTP_RX_WAIT);
	#if (appOPTIONS == 1)
		u8_t Option = ioB1GET(dbHTTPreq);
	#else
		#warning "Options support required for proper functioning!!!"
		u8_t Option = 0;
	#endif
	while((xRtosCheckStat1(sfREBOOT) == 0) && Mask) {
		u8_t optHost = 0;
		int iRV = erSUCCESS;
		s8_t BitNum	= __builtin_ctzl(Mask);				// identify next highest priority request
		Mask &= ~(1 << BitNum);							// remove request to be handled
		// prepare client request
		http_rr_t sRR = { 0 };
		sock_sec_t sSecure = { 0 };
		part_xfer_t	sPX = { 0 };
		http_parser sParser;
		http_parser_init(&sParser, HTTP_RESPONSE);
		sRR.sCtx.d = (u16_t) Option ? NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,3,1)
									: NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,3,0);
		psUBufCreate(&sRR.sUB, NULL, configHTTP_BUFSIZE, 0);
		sParser.data = &sRR;
		bool bOptName = 0, bOptHdlr;
		// now process the actual request...
		switch(BitNum) {
		case reqNUM_COREDUMP: {
			esp_core_dump_summary_t	sCDsummary = { 0 };
			sPX.psCDsum = &sCDsummary;
			iRV = esp_core_dump_get_summary(&sCDsummary);
			if (iRV == ESP_OK) iRV = esp_core_dump_image_get(&sPX.CDaddr, &sPX.CDsize);
			if (iRV == ESP_OK) {
				sPX.sIter = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
				IF_myASSERT(debugRESULT, sPX.sIter != 0);
				sPX.psPart = esp_partition_get(sPX.sIter);
				IF_myASSERT(debugRESULT, sPX.psPart != 0);

				#if (appOPTIONS == 1)
					optHost = ioB2GET(ioHostCONF);
				#else
					#warning "Options support required for proper functioning!!!"
					optHost = 0;
				#endif
				sRR.sCtx.pHost = HostInfo[optHost].pName;
				sSecure.pcCert = HostInfo[optHost].pcCert;
				sSecure.szCert = HostInfo[optHost].szCert;
				sRR.sCtx.psSec = &sSecure;
				#define httpCLNT_REQ_COREDUMP "PUT /coredump/%M_%X_%X_%lu.elf"
				uprintfx(&sRR.sUB, httpCLNT_REQ_COREDUMP, macSTA, esp_reset_reason(), buildFW_VER_NUM, xTimeStampSeconds(sTSZ.usecs));
				sRR.hdlr = halFlashUpload_CB;
				sRR.hvValues = httpHDR_VALUES(ctApplicationOctetStream, 0, 0, 0);
				sRR.hvContentLength = (u64_t) sPX.CDsize;
				sRR.pvArg = &sPX;						// Needed in upload handler				
			}
			break;
        }
		case reqNUM_FW_UPG1:
		case reqNUM_FW_UPG2:
			halEventUpdateStat1(sfFW_OK, 0);			// ONLY cleared for upgrades NOT checks
		case reqNUM_BL_UPG:
		case reqNUM_FW_CHK1:
		case reqNUM_FW_CHK2:
		case reqNUM_BL_CHK: {
			#if (appOPTIONS == 1)
				optHost = ioB2GET(ioHostFOTA);
			#else
				#warning "Options support required for proper functioning!!!"
				optHost = 0;
			#endif
			sRR.sCtx.pHost = HostInfo[optHost].pName;
			sSecure.pcCert = HostInfo[optHost].pcCert;
			sSecure.szCert = HostInfo[optHost].szCert;
			sRR.sCtx.psSec = &sSecure;
			if ((BitNum != reqNUM_BL_UPG) && (BitNum != reqNUM_BL_CHK)) {	// ensure NOT a bootloader option
				#define httpCLNT_REQ_FIRMWARE "GET /firmware/%s.bin"
				// Set flag for MAC vs UUID FW filename option
				bOptName = ((BitNum == reqNUM_FW_UPG1) || (BitNum == reqNUM_FW_CHK1)) ? 1 : 0;
				// Set flag for UPGrade or CHecK option ie select handler
				bOptHdlr = ((BitNum == reqNUM_FW_UPG1) || (BitNum == reqNUM_FW_UPG2)) ? 1 : 0;
				// Build request body
				uprintfx(&sRR.sUB, httpCLNT_REQ_FIRMWARE, bOptName ? (void *)idSTA : buildUUID);
				// Set correct handler
				sRR.sfCB.on_body = bOptHdlr ? xHttpClientDownload : xHttpClientCheckNewer;
				sPX.tLow = BuildSeconds;
				sPX.tDiff = 120;		// Required MIN difference (hvLastModified - BuildSeconds)
				sPX.psHdlr = &sHttpHdlrFOTA;
			} else {
				#define httpBOOT_REQ_FNAME "bootloader.bin"
				#define httpBOOT_REQ_LOADER "GET /firmware/%>s/%s"
				uprintfx(&sRR.sUB, httpBOOT_REQ_LOADER, buildMODEL, httpBOOT_REQ_FNAME);
				// Set correct handler
				sRR.sfCB.on_body = (BitNum == reqNUM_BL_UPG) ? xHttpClientDownload : xHttpClientCheckNewer;
				sPX.tLow = sFIB.fi[0].tBuild;
				sPX.tDiff = 10;		// Required MIN difference (hvLastModified - BuildSeconds)
				sPX.psHdlr = &sHttpHdlrBootloader;
				sPX.pcFName = httpBOOT_REQ_FNAME;					// name to use in FFS
			}
			sRR.hvValues = httpHDR_VALUES(ctTextPlain, ctApplicationOctetStream, coKeepAlive, 0);
			sRR.pvArg = &sPX;						// Needed in upload handler				
			break;
        }
		case reqNUM_GEOLOC:
		case reqNUM_GEOTZ:
		case reqNUM_GEOCODE:
		case reqNUM_GEOALT: {
			// all common to Google API's
			sSecure.pcCert = CertGGLE;
			sSecure.szCert = SizeGGLE;
			sRR.sCtx.psSec = &sSecure;
			sRR.sfCB.on_body = (http_data_cb) xHttpParseGeneric;
			if (BitNum == reqNUM_GEOLOC) {
				sRR.sCtx.pHost = "www.googleapis.com";
				#define httpCLNT_REQ_GEOLOC "POST /geolocation/v1/geolocate?key=%s"
				uprintfx(&sRR.sUB, httpCLNT_REQ_GEOLOC, keyGOOGLE);
				sRR.pcBody = "{ \"considerIp\":true }";
				sRR.hvValues = httpHDR_VALUES(ctApplicationJson, ctApplicationJson, 0, 0);
				sRR.pvArg = (void *) &saEntryGeoLoc;
			} else {
				sRR.sCtx.pHost = "maps.googleapis.com";
				sRR.hvValues = httpHDR_VALUES(ctTextPlain, ctApplicationJson, 0, 0);
				if (BitNum == reqNUM_GEOTZ) {
					sRR.pvArg = (void *) &saEntryGeoTZ;
					#define httpCLNT_GOOG_TZ "GET /maps/api/timezone/json?location=%.7f,%.7f&timestamp=%lu&key=%s"
					uprintfx(&sRR.sUB, httpCLNT_GOOG_TZ, sNVSvars.GeoLoc[geoLAT], sNVSvars.GeoLoc[geoLON], xTimeStampSeconds(sTSZ.usecs), keyGOOGLE);
				} else if (BitNum == reqNUM_GEOCODE) {
					sRR.pvArg = (void *) &saEntryGeoCode;
//					sRR.sCtx.d = NETX_DBG_FLAGS(0,0,0,1,0,0,0,1,1,0,0,0,0,3,1);
					#define httpCLNT_GOOG_CODE "GET /maps/api/geocode/json?latlng=%.7f,%.7f&key=%s&result_type=country"
					uprintfx(&sRR.sUB, httpCLNT_GOOG_CODE, sNVSvars.GeoLoc[geoLAT], sNVSvars.GeoLoc[geoLON], keyGOOGLE);
				} else {
					sRR.pvArg = (void *) &saEntryGeoAlt;
					#define httpCLNT_GOOG_ALT "GET /maps/api/elevation/json?locations=%.7f,%.7f&key=%s"
					uprintfx(&sRR.sUB, httpCLNT_GOOG_ALT, sNVSvars.GeoLoc[geoLAT], sNVSvars.GeoLoc[geoLON], keyGOOGLE);
				}
			}
			break;
        }
		default: break;
		}
		if (iRV < erSUCCESS)
			goto exit;
		IF_myASSERT(debugTRACK, sRR.hvContentType != ctUndefined);
		uprintfx(&sRR.sUB, " HTTP/1.1\r\nHost: %s\r\nFrom: admin@irmacos.com\r\nUser-Agent: irmacos\r\nAccept: %s\r\n",
			sRR.sCtx.pHost, ctValues[sRR.hvAccept]);
		sRR.hvAccept = ctUndefined;
		if (sRR.hvConnect) uprintfx(&sRR.sUB, "Connection: %s\r\n", coValues[sRR.hvConnect]);
		uprintfx(&sRR.sUB, "Content-Type: %s\r\n", ctValues[sRR.hvContentType]);
		if (sRR.pcBody && sRR.hvContentLength == 0)		// currently handle json/xml/text/html here
			sRR.hvContentLength = (u64_t) strlen(sRR.pcBody);
		if (sRR.hvContentLength) uprintfx(&sRR.sUB, "Content-Length: %llu\r\n", sRR.hvContentLength);
		uprintfx(&sRR.sUB, "\r\n");						// end of header fields, add blank line...
		if (sRR.pcBody) uprintfx(&sRR.sUB, "%s\r\n", sRR.pcBody);	// add actual content
		IF_PX(debugTRACK && Option && sRR.sCtx.d.http, "%'-+hhY", xUBufGetUsed(&sRR.sUB), pcUBufTellRead(&sRR.sUB));

		// Now start the network communication portion....
		sRR.sCtx.type = SOCK_STREAM;
		sRR.sCtx.sa_in.sin_family = AF_INET;
		if (sRR.sCtx.sa_in.sin_port == 0) sRR.sCtx.sa_in.sin_port = htons(sRR.sCtx.psSec ? IP_PORT_HTTPS : IP_PORT_HTTP);
		sRR.sCtx.flags = SO_REUSEADDR;
		
		IF_SYSTIMER_START(debugTIMING, stHTTP);
		iRV = xNetOpen(&sRR.sCtx);
		if (iRV == erSUCCESS) {							// if socket is open
			iRV = xNetSend(&sRR.sCtx, sRR.sUB.pBuf, sRR.sUB.Used);	// write request
			if (iRV > 0 && sRR.hdlr) {					// is handler specified for [additional] body
				iRV = sRR.hdlr(&sRR);					// send body (should return same as xNetSend...)
			}
			if (iRV > 0) {								// now read the response
				iRV = xNetRecv(&sRR.sCtx, sRR.sUB.pBuf, sRR.sUB.Size);
				if (iRV > 0) {							// actually read something
					sRR.sUB.Used = iRV;
					iRV = xHttpCommonDoParsing(&sParser);	// return erFAILURE or some 0+ number
				} else {
					iRV = erFAILURE;
				}
			} else {
				iRV = erFAILURE;
			}
		}
		IF_SYSTIMER_STOP(debugTIMING, stHTTP);
		xNetClose(&sRR.sCtx);							// close the socket connection if still open...
exit:
		vUBufDestroy(&sRR.sUB);							// return memory allocated
		switch(BitNum) {								// Do post processing
		case reqNUM_FW_UPG1:
		case reqNUM_FW_UPG2:
		{	if (xRtosCheckStat1(sfREBOOT))				// If reboot flag set we have new FW image
				Mask &= ~reqFW_UPGRADE;					// yes, abandon possible 2nd stage
			if ((Mask & reqFW_UPGRADE) == 0) {			// If UPGRADE (1 and/or 2) completed ?
				// If reboot flag not set then no new FW, mark FW as still valid/OK
				if (xRtosCheckStat1(sfREBOOT) == 0) halEventUpdateStat1(sfFW_OK, 1);
			}
		}	break;
		case reqNUM_FW_CHK1:
		case reqNUM_FW_CHK2:
			SL_WARN("New Firmware '%s' %s available", bOptName ? (void *)idSTA : buildUUID, (sRR.onBodyRet < 1) ? "NOT" : strNUL);
			break;
		case reqNUM_BL_UPG:
			SL_WARN("Bootloader '%s' upgrade %s", httpBOOT_REQ_FNAME, (sRR.onBodyRet < 1) ? "FAIL" : "successful");
			break;
		case reqNUM_BL_CHK:
			SL_WARN("New Bootloader '%s' %s available", httpBOOT_REQ_FNAME, (sRR.onBodyRet < 1) ? "NOT" : strNUL);
			break;
		default:
			break;
		}
	}
	vTaskDelete(TempHandle = NULL);
}

TaskHandle_t xHttpClientTaskStart(void * pvPara) {
	static StaticTask_t ttsHttpC = { 0 };
	static StackType_t tsbHttpC[httpCLNT_STACK_SIZE] = { 0 };
	const task_param_t sHttpParam = {
		.pxTaskCode = vTaskHttpClient,
		.pcName = "clnt",
		.usStackDepth = httpCLNT_STACK_SIZE,
		.uxPriority = httpCLNT_PRIORITY,
		.pxStackBuffer = tsbHttpC,
		.pxTaskBuffer = &ttsHttpC,
		.xCoreID = tskNO_AFFINITY,
		.xMask = taskHTTP_CLNT_MASK,
	};
	return xTaskCreateWithMask(&sHttpParam, pvPara);
}

#if 0
// ###################################### Various gateways #########################################

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
		"PATCH /ibuttons.dat", "{'%M' , 'DS1990R' , 'Heavy Duty' , 'Maxim' }""\r\n", NULL, 0, 0,
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

/*	TODO:
	Investigate counter for enums remove manual value checking

	http://wolkykim.github.io/libasyncd/
	https://www.gnu.org/software/libmicrohttpd/

*/