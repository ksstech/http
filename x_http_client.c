/*
 * Copyright 2014-23 (c) Andre M. Maree / KSS Technologies (Pty) Ltd.
 */

#include "hal_variables.h"			// required by options.h

#include "FreeRTOS_Support.h"
#include "hal_network.h"
#include "hal_fota.h"									// firmware download handler
#include "options.h"
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

#define configHTTP_BUFSIZE			4096
#define configHTTP_RX_WAIT			5000	// 500

#ifndef keyGOOGLE
	#define	keyGOOGLE				"fakegoogle"
#endif

// ################################ Google HTTPS Client Certificate ################################

const char CertGGLE[]	=
	"-----BEGIN CERTIFICATE-----\n"
	"MIIDIDCCAomgAwIBAgIENd70zzANBgkqhkiG9w0BAQUFADBOMQswCQYDVQQGEwJV\n"
	"UzEQMA4GA1UEChMHRXF1aWZheDEtMCsGA1UECxMkRXF1aWZheCBTZWN1cmUgQ2Vy\n"
	"dGlmaWNhdGUgQXV0aG9yaXR5MB4XDTk4MDgyMjE2NDE1MVoXDTE4MDgyMjE2NDE1\n"
	"MVowTjELMAkGA1UEBhMCVVMxEDAOBgNVBAoTB0VxdWlmYXgxLTArBgNVBAsTJEVx\n"
	"dWlmYXggU2VjdXJlIENlcnRpZmljYXRlIEF1dGhvcml0eTCBnzANBgkqhkiG9w0B\n"
	"AQEFAAOBjQAwgYkCgYEAwV2xWGcIYu6gmi0fCG2RFGiYCh7+2gRvE4RiIcPRfM6f\n"
	"BeC4AfBONOziipUEZKzxa1NfBbPLZ4C/QgKO/t0BCezhABRP/PvwDN1Dulsr4R+A\n"
	"cJkVV5MW8Q+XarfCaCMczE1ZMKxRHjuvK9buY0V7xdlfUNLjUA86iOe/FP3gx7kC\n"
	"AwEAAaOCAQkwggEFMHAGA1UdHwRpMGcwZaBjoGGkXzBdMQswCQYDVQQGEwJVUzEQ\n"
	"MA4GA1UEChMHRXF1aWZheDEtMCsGA1UECxMkRXF1aWZheCBTZWN1cmUgQ2VydGlm\n"
	"aWNhdGUgQXV0aG9yaXR5MQ0wCwYDVQQDEwRDUkwxMBoGA1UdEAQTMBGBDzIwMTgw\n"
	"ODIyMTY0MTUxWjALBgNVHQ8EBAMCAQYwHwYDVR0jBBgwFoAUSOZo+SvSspXXR9gj\n"
	"IBBPM5iQn9QwHQYDVR0OBBYEFEjmaPkr0rKV10fYIyAQTzOYkJ/UMAwGA1UdEwQF\n"
	"MAMBAf8wGgYJKoZIhvZ9B0EABA0wCxsFVjMuMGMDAgbAMA0GCSqGSIb3DQEBBQUA\n"
	"A4GBAFjOKer89961zgK5F7WF0bnj4JXMJTENAKaSbn+2kmOeUJXRmm/kEd5jhW6Y\n"
	"7qj/WsjTVbJmcVfewCHrPSqnI0kBBIZCe/zuf6IWUrVnZ9NA2zsmWLIodz2uFHdh\n"
	"1voqZiegDfqnc1zqcPGUIWVEX/r87yloqaKHee9570+sB3c4\n"
	"-----END CERTIFICATE-----\n";

// ################################### Common HTTP API functions ###################################

int	xHttpBuildHeader(http_parser * psParser) {
	http_rr_t * psRR = psParser->data;
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psParser) && halCONFIG_inSRAM(psRR) && halCONFIG_inSRAM(psRR->sUB.pBuf));

	vuprintfx(&psRR->sUB, psRR->pcQuery, psRR->VaList);
	uprintfx(&psRR->sUB, " HTTP/1.1\r\nHost: %s\r\nFrom: admin@kss.co.za\r\nUser-Agent: irmacos\r\n", psRR->sCtx.pHost);
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
				psRR->hvContentLength = vsprintfx(NULL, psRR->pcBody, psRR->VaList);	// determine body length
				uprintfx(&psRR->sUB, "Content-Length: %d\r\n\r\n", psRR->hvContentLength);
				vuprintfx(&psRR->sUB, psRR->pcBody, psRR->VaList);// add actual content
			}
		} else
			SL_ERR(debugAPPL_PLACE);
	}
	// add the final CR after the headers and payload, if binary payload this is 2nd strCRLF pair
	uprintfx(&psRR->sUB, strCRLF);
//	IF_P(debugTRACK && ioB1GET(ioHTTPtrack) && psRR->sCtx.d.http, "Content:\r\n%.*s\r\n", psRR->sUB.Used, psRR->sUB.pBuf);
//	IF_PX(debugTRACK && ioB1GET(ioHTTPtrack) && psRR->sCtx.d.http, "Content:\r\n%+-hhY\r\n", psRR->sUB.Used, psRR->sUB.pBuf);
	return psRR->sUB.Used;
}

/**
 * @brief	Build HTTP request packet and initiates the communications
 * @brief	Coordinate connecting, parsing, responding and disconnecting activities
 * @return	erFAILURE or result of xHttpCommonDoParsing() being 0 or more
 */
int	xHttpRequest(pcc_t pHost, pcc_t pQuery, const void * pvBody,
		pcc_t pcCert, size_t szCert,					// host certificate info
		void * OnBodyCB, u32_t DataSize,				// read/write handler & size
		u32_t hvValues, u16_t BufSize, netx_dbg_t Debug, void * pvArg, ...) {
	http_rr_t sRR		= { 0 };
	sock_sec_t sSecure	= { 0 };				// LEAVE here else pcCert/szCert gets screwed
	sRR.sCtx.pHost		= pHost;
	sRR.pcQuery			= pQuery;
	sRR.pVoid			= pvBody;
	if (pcCert) {
		sRR.sCtx.psSec	= &sSecure;
		sSecure.pcCert	= pcCert;
		sSecure.szCert	= szCert;
	}
	sRR.sfCB.on_body	= (http_data_cb) OnBodyCB;
	sRR.hvContentLength	= (u64_t) DataSize;
	sRR.hvValues		= hvValues;
	sRR.sUB.Size		= BufSize ? BufSize : configHTTP_BUFSIZE;
	sRR.pvArg			= pvArg;
	IF_P(debugTRACK && ioB1GET(ioHTTPtrack), "H='%s'  Q='%s'  cb=%p  hv=0x%08X  B=",
			sRR.sCtx.pHost, sRR.pcQuery, sRR.sfCB.on_body, sRR.hvValues);
	IF_P(debugTRACK && ioB1GET(ioHTTPtrack), sRR.hvContentType == ctApplicationOctetStream ? "%p\r\n" : "%s\r\n", sRR.pVoid);
	IF_myASSERT(debugTRACK, sRR.hvContentType != ctUNDEFINED);

	sRR.sCtx.d.val = Debug.val;
	IF_SYSTIMER_INIT(debugTIMING, stHTTP, stMILLIS, "HTTPclnt", configHTTP_RX_WAIT/100, configHTTP_RX_WAIT);
	IF_SYSTIMER_START(debugTIMING, stHTTP);
	http_parser sParser;
	http_parser_init(&sParser, HTTP_RESPONSE);			// clear all parser fields/values
	sParser.data = &sRR;

	// setup the ubuf_t structure for printing
	psUBufCreate(&sRR.sUB, NULL, sRR.sUB.Size, 0);

	va_list vArgs;
	va_start(vArgs, pvArg);
	sRR.VaList = vArgs;
	int xLen = xHttpBuildHeader(&sParser);
	va_end(vArgs);

	sRR.sCtx.type = SOCK_STREAM;
	sRR.sCtx.sa_in.sin_family	= AF_INET;
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
					IF_P(debugTRACK && ioB1GET(ioHTTPtrack), " nothing read ie to parse\r\n");
					iRV = erFAILURE;
				}
			} else {
				IF_P(debugTRACK && ioB1GET(ioHTTPtrack), " nothing written (by handler) so can't expect to read\r\n");
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
	jsmntok_t *	psTokenList;
	int	NumTok, iRV = erFAILURE;
	const char * pKey = " Insufficient";
	NumTok = xJsonParse(pcBuf, xLen, &sParser, &psTokenList);
	if (NumTok > 0) {									// parse Latitude
		iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "lat", (px_t) &sNVSvars.GeoLocation[geoLAT], cvF32);
		if (iRV >= erSUCCESS) {							// parse Longitude
			iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "lng", (px_t) &sNVSvars.GeoLocation[geoLON], cvF32);
			if (iRV >= erSUCCESS)						// parse accuracy
				iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "accuracy", (px_t) &sNVSvars.GeoLocation[geoACC], cvF32);
		}
	}
	if (iRV >= erSUCCESS) {
		setSYSFLAGS(vfGEOLOC);
		SL_NOT("lat=%.7f  lng=%.7f  acc=%.7f", sNVSvars.GeoLocation[geoLAT],
				sNVSvars.GeoLocation[geoLON], sNVSvars.GeoLocation[geoACC]);
		IF_EXEC_4(debugTRACK && ioB1GET(dbgHTTPreq), xJsonPrintTokens, pcBuf, psTokenList, NumTok, 0);
	}
	if (psTokenList)
		vRtosFree(psTokenList);
    return iRV;
}

int	xHttpGetLocation(void) {
	const char caQuery[] = "POST /geolocation/v1/geolocate?key="keyGOOGLE;
	return xHttpRequest("www.googleapis.com", caQuery, "{ }\r\n",
			CertGGLE, sizeof(CertGGLE), xHttpParseGeoLoc, 0,
			httpHDR_VALUES(ctApplicationJson, ctApplicationJson, 0, 0),
			0, NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0), NULL);
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
	jsmntok_t *	psTokenList;
	int	NumTok, iRV = erFAILURE;
	const char * pKey = " Insufficient";
	NumTok = xJsonParse(pcBuf, xLen, &sParser, &psTokenList);
	if (NumTok > 0) {
		x32_t	xVal;
		iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "dstOffset", (px_t) &xVal.i32, cvI32);
		if (iRV >= erSUCCESS) {
			sNVSvars.sTZ.daylight = xVal.i32;			// convert i32 -> i16 & store
			iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "rawOffset", (px_t) &xVal.i32, cvI32);
			if (iRV >= erSUCCESS) {
				sNVSvars.sTZ.timezone = xVal.i32;		// store value
				iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "timeZoneId", (px_t) sNVSvars.sTZ.TZid, cvSXX);
				if (iRV >= erSUCCESS)
					iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "timeZoneName", (px_t) sNVSvars.sTZ.TZname, cvSXX);
			}
		}
	}
	if (iRV >= erSUCCESS) {
		setSYSFLAGS(vfGEOTZ);
		SL_NOT("%Z(%s)", &sTSZ, sTSZ.pTZ->TZname);
		IF_EXEC_4(debugTRACK && ioB1GET(dbgHTTPreq), xJsonPrintTokens, pcBuf, psTokenList, NumTok, 0);
	}
	if (psTokenList)
		vRtosFree(psTokenList);
    return iRV;
}

int	xHttpGetTimeZone(void) {
	char const * caQuery = "GET /maps/api/timezone/json?location=%.7f,%.7f&timestamp=%d&key="keyGOOGLE;
	return xHttpRequest("maps.googleapis.com", caQuery, NULL,
			CertGGLE, sizeof(CertGGLE), xHttpParseTimeZone, 0,
			httpHDR_VALUES(ctTextPlain, ctApplicationJson, 0, 0),
			0, NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0), NULL,
			sNVSvars.GeoLocation[geoLAT], sNVSvars.GeoLocation[geoLON]);
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
	jsmntok_t *	psTokenList;
	int NumTok = xJsonParse(pcBuf, xLen, &sParser, &psTokenList);
	if (NumTok > 0) {									// parse Elevation
		iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "elevation", (px_t) &sNVSvars.GeoLocation[geoALT], cvF32);
		if (iRV >= erSUCCESS)							// parse Resolution
			iRV = xJsonParseKeyValue(pcBuf, psTokenList, NumTok, pKey = "resolution", (px_t) &sNVSvars.GeoLocation[geoRES], cvF32);
	}
	if (iRV >= erSUCCESS) {
		setSYSFLAGS(vfGEOALT);
		SL_NOT("alt=%.7f  res=%.7f", sNVSvars.GeoLocation[geoALT], sNVSvars.GeoLocation[geoRES]);
		IF_EXEC_4(debugTRACK && ioB1GET(dbgHTTPreq), xJsonPrintTokens, pcBuf, psTokenList, NumTok, 0);
	}
	if (psTokenList)
		vRtosFree(psTokenList);
    return iRV;
}

int	xHttpGetElevation(void) {
	const char caQuery[] = "GET /maps/api/elevation/json?locations=%.7f,%.7f&key="keyGOOGLE;
	return xHttpRequest("maps.googleapis.com", caQuery, NULL,
			CertGGLE, sizeof(CertGGLE), xHttpParseElevation, 0,
			httpHDR_VALUES(ctTextPlain, ctApplicationJson, 0, 0),
			0, NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0), NULL,
			sNVSvars.GeoLocation[geoLAT], sNVSvars.GeoLocation[geoLON]);
}

// ################################# Firmware Over The Air support #################################

/**
 * Check if a valid firmware upgrade exists
 * @return	erFAILURE if file not found/empty file/invalid content/connection closed
 * 			0 if no newer upgrade file exists
 * 			1 if valid upgrade file exists
 */
static int	xHttpClientCheckFOTA(http_parser * psParser, const char * pBuf, size_t xLen) {
	http_rr_t * psRR = psParser->data;
	int iRV = erFAILURE;
	if (psParser->status_code != HTTP_STATUS_OK) {
		IF_SL_WARN(debugTRACK && ioB1GET(ioFOTA), "'%s' Error=%d", psRR->pvArg, psParser->status_code);
	} else if (psRR->hvContentLength == 0ULL) {
		SL_ERR("'%s' invalid size (%llu)", psRR->pvArg, psRR->hvContentLength);
	} else if (psRR->hvContentType != ctApplicationOctetStream) {
		SL_ERR("'%s' invalid content (%d/%s)", psRR->pvArg, psRR->hvContentType, ctValues[psRR->hvContentType]);
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
		IF_SL_NOT(debugTRACK && ioB1GET(ioFOTA), "'%s' found  %R vs %R  Diff=%ld  FW %s",
				psRR->pvArg, xTimeMakeTimestamp(psRR->hvLastModified, 0),
				xTimeMakeTimestamp(BuildSeconds, 0), i32Diff, i32Diff < 0 ? "old" : "new");
	}
	return iRV;
}

/**
 * Check if a valid download exists, if so, download and write to flash.
 * @param	psParser
 * @param	pBuf
 * @param	xLen
 * @return
 */
static int xHttpClientPerformFOTA(http_parser * psParser, const char * pBuf, size_t xLen) {
	int iRV = xHttpClientCheckFOTA(psParser, pBuf, xLen);
	if (iRV < 1)					// 1=NewFW  0=LatestFW  -1=Error
		return iRV;
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
	iRV = halFOTA_End(&sFI);
	if (iRV == erSUCCESS && sFI.iRV == ESP_OK)
		setSYSFLAGS(sfREBOOT);
	return sFI.iRV;
}

static int	xHttpClientFirmwareUpgrade(void * pvPara, bool bCheck) {
	return xHttpRequest(HostInfo[ioB2GET(ioHostFOTA)].pName, "GET /firmware/%s.bin", NULL,
			HostInfo[ioB2GET(ioHostFOTA)].pcCert,
			HostInfo[ioB2GET(ioHostFOTA)].szCert,
			bCheck == CHECK ? xHttpClientCheckFOTA : xHttpClientPerformFOTA, 0,
			httpHDR_VALUES(ctTextPlain, ctApplicationOctetStream, coKeepAlive, 0),
			0, NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0), pvPara, pvPara);
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
		iRV = xHttpClientFirmwareUpgrade((void *) mySTRINGIFY(cmakeUUID), bCheck);
	if (bCheck == PERFORM)
		SL_LOG(iRV < erSUCCESS ? SL_SEV_ERROR : SL_SEV_NOTICE, "FWupg %s", iRV < erSUCCESS ? "FAIL" : "Done");
	if (allSYSFLAGS(sfREBOOT) == 0)
		setSYSFLAGS(sfFW_OK);
	return iRV;
}

// ################################## PUT core dump to host ########################################

// for binary uploads the address and content length+type must be correct
int xHttpCoredumpUpload(void) {
	#ifdef CONFIG_ESP_COREDUMP_DATA_FORMAT_BIN
		const char caQuery[] = "PUT /coredump/%M_%X_%X_%llu.bin" ;
//	#elif defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
	#else
		const char caQuery[] = "PUT /coredump/%M_%X_%X_%llu.elf" ;
	#endif
	esp_partition_iterator_t sIter;
	sIter = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
	IF_myASSERT(debugRESULT, sIter != 0);

	const esp_partition_t *	psPart = esp_partition_get(sIter);
	IF_myASSERT(debugRESULT, psPart != 0);

	struct cd_hdr_s { u32_t data_len, version, tasks_num, tcb_sz; } sCDhdr;
	int iRV = esp_partition_read(psPart, 0, &sCDhdr, sizeof(struct cd_hdr_s));
	SL_WARN("iRV=%d  L=%u  T=%u  TCB=%u  V=%-I", iRV, sCDhdr.data_len, sCDhdr.tasks_num, sCDhdr.tcb_sz, sCDhdr.version);

	if (iRV == ESP_OK && (sCDhdr.data_len != sCDhdr.tasks_num && sCDhdr.tcb_sz != sCDhdr.version)) {
		iRV = xHttpRequest(HostInfo[ioB2GET(ioHostCONF)].pName, caQuery, halPART_Upload_CB,
			HostInfo[ioB2GET(ioHostFOTA)].pcCert,HostInfo[ioB2GET(ioHostFOTA)].szCert,
			NULL, sCDhdr.data_len,
			httpHDR_VALUES(ctApplicationOctetStream, 0, 0, 0),
			0, NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,1), (void *) psPart,
			macSTA, esp_reset_reason(), DEV_FW_VER_NUM, sTSZ.usecs/MICROS_IN_SECOND);
	}
	esp_partition_iterator_release(sIter);
	return iRV;
}

// ###################################### Various gateways #########################################

#ifndef	tokenPUSHOVER
	#define	tokenPUSHOVER "faketoken"
#endif

#ifndef	userPUSHOVER
	#define	userPUSHOVER "fakeuser"
#endif

int	xHttpClientPushOver(const char * pcMess, u32_t u32Val) {
	const char caBody[] = "token="tokenPUSHOVER "&user="userPUSHOVER "&title=%U&message=%U%%40%u";
	return xHttpRequest("api.pushover.net", "POST /1/messages.json", caBody,
			CertGGLE, sizeof(CertGGLE), NULL, 0,
			httpHDR_VALUES(ctApplicationXwwwFormUrlencoded, ctApplicationJson, 0, 0),
			0, NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,1), NULL,
			nameSTA, pcMess, u32Val);
}

// ################################## POST IDENT info to host ######################################

/**
 * xHttpClientIdentUpload - Upload single DS1990 tag info to cloud
 * @param[in]	pointer to tag ROM ID string
 */
int	xHttpClientIdentUpload(void * psRomID) {
	return xHttpRequest(HostInfo[ioB2GET(ioHostCONF)].pName, "PATCH /ibuttons.dat",
			"'%M' , 'DS1990R' , 'Heavy Duty' , 'Maxim'\r\n",
			NULL, 0, 						// certificate info
			NULL, 0, 						// read/write handler & size
			httpHDR_VALUES(ctTextPlain, 0, 0, 0),
			0, NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,1), NULL, psRomID);
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
	const char caQuery[] = "GET /data/2.5/forecast/?q=Johannesburg,ZA&APPID=cf177bb6e86c95045841c63e99ad2ff4";
	return xHttpRequest("api.openweathermap.org", caQuery, NULL,
			NULL, 0, NULL, 0,
			httpHDR_VALUES(ctTextPlain,0,0,0),
			16384, NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,1), NULL);
}

// ################################### How's my SSL support ########################################

int	xHttpHowsMySSL(void) {
	return xHttpRequest("www.howsmyssl.com", "GET /a/check", NULL,
			HostInfo[ioB2GET(ioHostFOTA)].pcCert, HostInfo[ioB2GET(ioHostFOTA)].szCert, NULL, 0,
			httpHDR_VALUES(ctTextPlain,0,0,0),
			0, NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,1), NULL);
}

// ####################################### Bad SSL support #########################################

int	xHttpBadSSL(void) {
	return xHttpRequest("www.badssl.com", "GET /dashboard", NULL,
			HostInfo[ioB2GET(ioHostFOTA)].pcCert, HostInfo[ioB2GET(ioHostFOTA)].szCert, NULL, 0,
			httpHDR_VALUES(ctTextPlain,0,0,0),
			0, NETX_DBG_FLAGS(0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,1), NULL);
}

