/*
 * x_http_server.c s
 * Copyright (c) 2014-22 Andre M. Maree / KSS Technologies (Pty) Ltd.
 */

#include "hal_variables.h"			// required by options.h

#include "commands.h"
#include "hal_stdio.h"
#include "hal_network.h"
#include "hal_fota.h"
#include "hal_storage.h"
#include "hal_mcu.h"									// for halMCU_Restart()
#include "options.h"
#include "printfx.h"
#include "rules.h"
#include "syslog.h"
#include "task_control.h"
#include "x_errors_events.h"
#include "x_http_client.h"								// for xHttpFirmware????()
#include "x_http_server.h"
#include "x_string_general.h"
#include "x_string_to_values.h"
#include "x_time.h"

// ############################### BUILD: debug configuration options ##############################

#define	debugFLAG					0xF000

#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ########################################### macros ##############################################

#define halNET_EXTEND_IP			0
#define	httpINTERVAL_MS				(100)
#define httpSERVER_BUFSIZE			(1 * KILO)

// ###################################### enumerations #############################################

enum {
	urlROOT,
	urlSAVE_AP,
	urlAPI,
	urlNOTFOUND,
	urlNUMBER,							// Insert ALL URL endpoints above here
} ;

// ###################################### local ie static variables ################################

static	const char * const UrlTable[] = {
	[urlROOT]		= "/",
	[urlSAVE_AP]	= "save_ap",
	[urlAPI]		= "api",
} ;

static const char HtmlSTAdetails[] =
	"<html><head><title>'IRMACOS: STA details'</title></head><body>"
	"<h3>'IRMACOS: STA details'</h3>"
	"<p>Manufacturer    : " halDEV_VENDOR "</p>"
	"<p>Platform        : " halDEV_MODEL "</p>"
	"<p>Wifi FW version : " mySTRINGIFY(DEV_WIFI_VER) "</p>"
	"<p>Firmware Ver#   : " DEV_FW_VER_STR "</p>"
	"<form action='sta_update' method='get'><input type='submit' value='STA_Update'></form>"
	"</body></html>" ;

static const char HtmlAPdetails[] =
	"<html><head><title>'IRMACOS: AP details'</title></head>"
	"<body><h3>'IRMACOS: AP details'</h3>"
	"<form action='save_ap' method='get'>"
	"AP SSID (Max 32 chars incl A-Z, a-z and 0-9):<br><input type='text' name='ssid' size='32'><br>"
	"AP PSWD (Max 64 chars incl A-Z, a-z and 0-9):<br><input type='text' name='pswd' size='64'><br>"
#if		(halNET_EXTEND_IP == 1)
	"IP NetMask (w.x.y.z):<br><input type='text' name='nm' size='16' value='255.255.255.0'><br>"
	"IP Gateway (w.x.y.z):<br><input type='text' name='gw' size='16' value='192.168.1.1.'><br>"
	"IP Address (w.x.y.z):<br><input type='text' name='ip' size='16' value='192.168.1.100'><br>"
	"IP DNS#1 (w.x.y.z):<br><input type='text' name='d1' size='16' value='192.169.1.1'><br>"
	"IP DNS#2 (w.x.y.z):<br><input type='text' name='d2' size='16' value='8.8.8.8'><br>"
#endif
	"IP MQTT (w.x.y.z):<br><input type='text' name='mqtt' size='16' value='0.0.0.0'><br>"
	"<br><input type='submit' value='Submit'>"
	"</form></body></html>" ;

static const char HtmlAPconfigOK[] =
	"<html><body><h3>IRMACOS: AP details set, rebooting now..</h3>"
	"<p>Please wait a minute and then confirm success</p></body></html>" ;

static const char HtmlAPconfigFAIL[] =
	"<html><body><h3>IRMACOS: AP details NOT set, try again..</h3>"
	"<p>Please make sure only using the allowed characters</p></body></html>" ;

static const char HtmlErrorNoHost[] =
	"<html><body><h2>No Host: header received</h2>"
	"<p>HTTP 1.1 requests must include the Host: header.</p></body></html>" ;

static const char HtmlErrorInvMethod[] =
	"<html><body><h2>Invalid method request</h2>"
	"<p>IRMACOS only support GET, not HEAD, POST, DELETE etc..</p></body></html>" ;

static const char HtmlErrorBadQuery[] =
	"<html><body><h2>Query key:value pair(s) mismatched</h2></body></html>" ;

StaticTask_t ttsHTTP = { 0 };
StackType_t tsbHTTP[httpSTACK_SIZE] = { 0 };

uint8_t		HttpState ;
netx_t		sServHttpCtx ;
http_rr_t	sRR = { 0 } ;
static u32_t fRqst = 0;

// ###################################### global variables #########################################


// ################################## local/static support functions ###############################

static int xHttpServerSetResponseStatus(http_parser * psParser, int Status) {
	http_rr_t * psRR	= psParser->data ;
	psParser->status_code	= Status ;

	// set common defaults for responses
	psRR->hvContentType		= ctTextHtml ;
	psRR->hvAccept			= ctUNDEFINED ;
	psRR->hvConnect			= coClose ;

	switch(Status) {
	case HTTP_STATUS_OK:				psRR->pcStatMes	= "OK" ;				break ;	// 200
	case HTTP_STATUS_BAD_REQUEST:		psRR->pcStatMes	= "Bad Request" ;		break ;	// 400
	case HTTP_STATUS_NOT_FOUND:			psRR->pcStatMes	= "Not Found" ;			break ;	// 404
	case HTTP_STATUS_NOT_ACCEPTABLE:	psRR->pcStatMes	= "Not Acceptable" ;	break ;	// 406
	case HTTP_STATUS_NOT_IMPLEMENTED:	psRR->pcStatMes	= "Not Implemented" ;	break ;	// 501
	default:							SL_ERR(debugAPPL_PLACE) ;
	}
	if (INRANGE(HTTP_STATUS_BAD_REQUEST, Status, HTTP_STATUS_UNAVAILABLE_FOR_LEGAL_REASONS))
		psRR->hvConnect = coClose ;					// force connection to be closed
	return erSUCCESS ;
}

static int xvHttpSendResponse(void * pV, const char * format, va_list vaList) {
	http_parser * psParser = pV;
	http_rr_t * psRR = psParser->data ;
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psParser) && halCONFIG_inSRAM(psRR) && halCONFIG_inSRAM(psRR->sUB.pBuf)) ;
	int iRV ;
	iRV = socprintfx(&psRR->sCtx, "HTTP/1.1 %d %s\r\n", psParser->status_code, psRR->pcStatMes) ;
	iRV += socprintfx(&psRR->sCtx, "Date: %#Z\r\n", &sTSZ) ;
	iRV += socprintfx(&psRR->sCtx, "Content-Language: en-US\r\n") ;
	if (psRR->hvConnect) {
		iRV += socprintfx(&psRR->sCtx, "Connection: %s\r\n", coValues[psRR->hvConnect]) ;
		if (psRR->hvConnect == coKeepAlive)
			iRV += socprintfx(&psRR->sCtx, "Keep-Alive: timeout=3\r\n");
	}
	if (psRR->hvContentType) {
		iRV += socprintfx(&psRR->sCtx, "Content-Type: %s\r\n", ctValues[psRR->hvContentType]);
	}
	/* The next line does not really print anything. By specifying the NULL as the string
	 * pointer it performs a test print and simply return the number of characters that
	 * would have been printed if a destination was specified.*/
	psRR->hvContentLength = vsprintfx(NULL, format, vaList) ;
	/* Now do the actual formatted print of the content length with an additional 2 chars
	 * added for the extra CR + LF pair to form the blank line after the header values */
	iRV += socprintfx(&psRR->sCtx, "Content-Length: %d\r\n\r\n", psRR->hvContentLength + 2) ;
	iRV += vsocprintfx(&psRR->sCtx, format, vaList) ;				// add the actual content
	iRV += socprintfx(&psRR->sCtx, strCRLF) ;						// add the final CR+LF after the body
	IF_P(debugTRACK && ioB1GET(ioHTTPtrack) && psRR->sCtx.d.http, "Content:\r\n%.*s", psRR->sUB.Used, psRR->sUB.pBuf);
	return iRV;
}

static int xHttpSendResponse(http_parser * psParser, const char * format, ...) {
	va_list vaList;
	va_start(vaList, format) ;
	int iRV = xvHttpSendResponse(psParser, format, vaList);
	va_end(vaList);
	return iRV ;
}

static int xHttpServerParseString(char * pVal, char * pDst) {
	if (xStringParseEncoded(pDst, pVal) == erFAILURE) {
		return erFAILURE;
	}
	IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), "%s->%s\r\n", pVal, pDst) ;
	return erSUCCESS ;
}

static int xHttpServerParseIPaddress(char * pSrc, u32_t * pDst) {
	if (xStringParseEncoded(NULL, pSrc) == erFAILURE) {
		return erFAILURE;
	}
	IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), "%s", pSrc) ;
	if (pcStringParseIpAddr(pSrc, (px_t) pDst) == pcFAILURE) {
		*pDst = 0 ;
		return erFAILURE ;
	}
	IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), " : 0x%08X\r\n", *pDst) ;
	return erSUCCESS ;
}

// ######################################## URL handlers ###########################################

static int xHttpHandle_API(http_parser * psParser) {
	http_rr_t * psRR = psParser->data;
	// XXX This version will ONLY handle the 1st part of the command(s) received.....
	xStringParseEncoded(NULL, psRR->parts[1]);
	IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), "'%s'\r\n", psRR->parts[1]);
	int iRV = xCommandProcessString(psRR->parts[1], 0, xvHttpSendResponse, (void *) psParser,
		"<html><body><h2>Result</h2><pre>%.*s</pre></body></html>", xStdioBufAvail(), pcStdioBufTellRead());
	vStdioBufReset();
	return iRV;
}

// ################################### Common HTTP API functions ###################################

static void vTaskHttpCloseServer(void) {
	xRtosClearStatus(flagHTTP_SERV);
	int iRV = (sServHttpCtx.sd > 0) ? xNetClose(&sServHttpCtx) : 0;
	HttpState = stateHTTP_INIT ;
	IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), "server closed (%d)\r\n", iRV) ;
}

static void vTaskHttpCloseClient(void) {
	xRtosClearStatus(flagHTTP_CLNT);
	int iRV = (sRR.sCtx.sd > 0) ? xNetClose(&sRR.sCtx) : 0;
	HttpState = stateHTTP_WAITING;
	IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), "client closed (%d)\r\n", iRV);
}

static void vTaskHttpDeInit(void) {
	vTaskHttpCloseClient();
	vTaskHttpCloseServer();
}

/**
 * xHttpResponseHandler() - called after header (and content) parsing has completed
 * @comment Needs to use info from the parsing stage to build the response
 * @param psParser
 * @return	size of the response created (bytes)
 */
static int xHttpServerResponseHandler(http_parser * psParser) {
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psParser) && halCONFIG_inSRAM(psParser->data)) ;
	http_rr_t * psRR = psParser->data ;
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psRR->sUB.pBuf)) ;

	int	iURL = -1, iRV, i ;
	if (psParser->http_errno) {
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_NOT_ACCEPTABLE) ;
		psRR->pcBody = (char *) http_errno_description(HTTP_PARSER_ERRNO(psParser)) ;
		SL_ERR("%s (%s)", psRR->pcBody, http_errno_name(HTTP_PARSER_ERRNO(psParser))) ;
		psRR->hvContentType = ctTextPlain ;

	} else if (psParser->method != HTTP_GET) {			// Invalid METHOD
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_NOT_IMPLEMENTED) ;
		psRR->pcBody = (char *) HtmlErrorInvMethod ;
		SL_ERR("Method not supported (%d)", psParser->method) ;

	} else if (psRR->f_host == 0) {						// host not provided
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_BAD_REQUEST) ;
		psRR->pcBody = (char *) HtmlErrorNoHost ;
		SL_ERR("Host name/IP not provided") ;

	} else {		// at this stage all parsing results are OK, just the URL to be matched and processed.
		if (*psRR->url.path == 0) {						// STEP1: start by matching the URL
			iURL = urlROOT ;							// do NOT try to match, lost single '/'
		} else {
			iURL = xHttpCommonFindMatch(UrlTable, NO_MEM(UrlTable), psRR->url.path, strlen(psRR->url.path)) ;
			if (iURL == 0)
				iURL = urlNOTFOUND ;
		}
	}

	switch(iURL) {
	case urlROOT:
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_OK) ;
		psRR->pcBody = (WLstate.CurSTA) ? (char *) HtmlSTAdetails : (char *) HtmlAPdetails ;
		break ;

	case urlSAVE_AP:
		i = 0 ;
		if ((strcmp(psRR->params[i++].key, halSTORAGE_KEY_SSID) != 0)	||
			(strcmp(psRR->params[i++].key, halSTORAGE_KEY_PSWD) != 0)	||
			#if	(halNET_EXTEND_IP == 1)
			(strcmp(psRR->params[i++].key, halSTORAGE_KEY_NM)	!= 0)	||
			(strcmp(psRR->params[i++].key, halSTORAGE_KEY_GW)	!= 0)	||
			(strcmp(psRR->params[i++].key, halSTORAGE_KEY_IP)	!= 0)	||
			(strcmp(psRR->params[i++].key, halSTORAGE_KEY_DNS1) != 0)	||
			(strcmp(psRR->params[i++].key, halSTORAGE_KEY_DNS2) != 0)	||
			#endif
			(strcmp(psRR->params[i++].key, halSTORAGE_KEY_MQTT) != 0)) {
			// missing/corrupted/wrong key for an IP parameter, stop parsing & complain...
			xHttpServerSetResponseStatus(psParser, HTTP_STATUS_BAD_REQUEST) ;
			psRR->pcBody	= (char *) HtmlErrorBadQuery ;

		} else {
			nvs_wifi_t tmpWifi = { 0 } ;
			i = 0 ;
			// all IP parameter keys matched, parse them
			iRV = xHttpServerParseString(psRR->params[i++].val, (char *) tmpWifi.ap_info[0].ssid) ;
			if (iRV == erSUCCESS)
				iRV = xHttpServerParseString(psRR->params[i++].val, (char *) tmpWifi.ap_info[0].pswd);
			#if	(halNET_EXTEND_IP == 1)
			if (iRV == erSUCCESS)
				iRV = xHttpServerParseIPaddress(psRR->params[i++].val, &tmpWifi.ipNM);
			if (iRV == erSUCCESS)
				iRV = xHttpServerParseIPaddress(psRR->params[i++].val, &tmpWifi.ipGW);
			if (iRV == erSUCCESS)
				iRV = xHttpServerParseIPaddress(psRR->params[i++].val, &tmpWifi.ipSTA);
			if (iRV == erSUCCESS)
				iRV = xHttpServerParseIPaddress(psRR->params[i++].val, &tmpWifi.ipDNS1);
			if (iRV == erSUCCESS)
				iRV = xHttpServerParseIPaddress(psRR->params[i++].val, &tmpWifi.ipDNS2);
			#endif
			if (iRV == erSUCCESS)
				iRV = xHttpServerParseIPaddress(psRR->params[i++].val, &tmpWifi.ipMQTT);
			// Check if values allow successful Wifi connection (& persist if successful)
			iRV = halWL_TestCredentials(-1, (char *) tmpWifi.ap_info[0].ssid, (char *) tmpWifi.ap_info[0].pswd) ;
			if (iRV == erSUCCESS) {						// inform client of success or not....
				xHttpServerSetResponseStatus(psParser, HTTP_STATUS_OK) ;
				psRR->pcBody	= (char *) HtmlAPconfigOK ;
				setSYSFLAGS(sfREBOOT);
			} else {
				xHttpServerSetResponseStatus(psParser, HTTP_STATUS_NOT_ACCEPTABLE) ;
				psRR->pcBody	= (char *) HtmlAPconfigFAIL ;
			}
		}
		break ;

	case urlAPI:
		if (psRR->NumParts == 2) {
			xHttpServerSetResponseStatus(psParser, HTTP_STATUS_OK) ;
			psRR->f_bodyCB 	= 1 ;
			psRR->hdlr_rsp	= xHttpHandle_API ;
		} else {
			xHttpServerSetResponseStatus(psParser, HTTP_STATUS_BAD_REQUEST) ;
			psRR->pcBody 	= "<html><body><h2>** API command option Required **</h2></body></html>" ;
		}
		break ;

	case urlNOTFOUND:
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_NOT_FOUND) ;
		psRR->pcBody 	= "<html><body><h2>** URL Nor found **</h2></body></html>" ;
		break ;

	default:
		break ;
	}

	if (psRR->f_bodyCB && psRR->hdlr_rsp) {
		iRV = psRR->hdlr_rsp(psParser) ;			// Add dynamic content to buffer via callback
	} else {
		iRV = xHttpSendResponse(psParser, psRR->pcBody);
		IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), "Response sent iRV=%d\r\n", iRV);
	}
	if (sServHttpCtx.maxTx < iRV) {
		sServHttpCtx.maxTx = iRV;
	}
	return iRV ;
}

static void vHttpNotifyHandler(void) {
	u32_t fDone = 0;
	int iRV;
	if (xTaskNotifyWait(0, 0, &fRqst, 0) == pdTRUE) {
		IF_PL(debugTRACK && ioB1GET(ioHTTPtrack), "Received Notify 0x%06X\r\n", fRqst);
		if (fRqst & reqCOREDUMP) {
			xHttpCoredumpUpload();
			fDone |= reqCOREDUMP;
		}
		if (fRqst & reqFW_UPGRADE) {
			xRtosClearStateRUN(taskGUI_MASK);
			xHttpClientCheckUpgrades(PERFORM);
			xRtosSetStateRUN(taskGUI_MASK);
			fDone |= reqFW_UPGRADE;
		}
		if (fRqst & reqFW_CHECK) {
			xHttpClientCheckUpgrades(CHECK);
			fDone |= reqFW_CHECK;
		}
		if (allSYSFLAGS(sfREBOOT) == 0) {
			if (fRqst & reqGEOLOC) {
				iRV = xHttpGetLocation();
				if (iRV > erFAILURE)
					fDone |= reqGEOLOC;
			}
			if (fRqst & reqGEOTZ) {
				iRV = xHttpGetTimeZone();
				if (iRV > erFAILURE)
					fDone |= reqGEOTZ;
			}
			if (fRqst & reqGEOALT) {
				iRV = xHttpGetElevation();
				if (iRV > erFAILURE)
					fDone |= reqGEOALT;
			}
		} else if (fRqst & (reqGEOLOC | reqGEOTZ | reqGEOALT)) {
			fDone |= (reqGEOLOC | reqGEOTZ | reqGEOALT);
			SL_NOT("GeoXXX discarded, need to restart");
		}
		if (fDone) {
			SL_NOT("fRqst=0x%X  fDone=0x%X\r\n", fRqst, fDone);
			ulTaskNotifyValueClear(NULL, fDone);
		}
	}
}

/**
 *vTaskHttp()
 * @param pvParameters
 *
 * Responsibilities:
 * 	Serve HTML to capture SSID & PSWD from client
 * 	Respond to /restart (as emergency)
 */
static void vHttpTask(void * pvParameters) {
	int	iRV, iRV2;
	vTaskSetThreadLocalStoragePointer(NULL, buildFRTLSP_EVT_MASK, (void *)taskHTTP_MASK);
	sRR.sUB.pBuf = pvRtosMalloc(sRR.sUB.Size = httpSERVER_BUFSIZE);
	HttpState = stateHTTP_INIT;
	xRtosSetStateRUN(taskHTTP_MASK) ;

	while (bRtosVerifyState(taskHTTP_MASK)) {
		if (bRtosCheckStatus(flagLX_STA) == 0) {
			vTaskDelay(10);
			continue;
		}
		// Handle HTTP client type requests from other tasks
		vHttpNotifyHandler();

		switch(HttpState) {
		case stateHTTP_DEINIT:
			vTaskHttpDeInit();
			break ;					// must NOT fall through since the Lx status might have changed

		case stateHTTP_INIT:
			IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), "init\r\n") ;
			memset(&sServHttpCtx, 0 , sizeof(sServHttpCtx)) ;
			sServHttpCtx.sa_in.sin_family	= AF_INET ;
			sServHttpCtx.type				= SOCK_STREAM ;
			sServHttpCtx.sa_in.sin_port		= htons(IP_PORT_HTTP) ;
			sServHttpCtx.flags				= SO_REUSEADDR;
			iRV = xNetOpen(&sServHttpCtx) ;
			if (iRV < erSUCCESS) {
				HttpState = stateHTTP_DEINIT ;
				break ;
			}
			xRtosSetStatus(flagHTTP_SERV) ;
			HttpState = stateHTTP_WAITING ;
			IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), "waiting\r\n") ;
			/* FALLTHRU */ /* no break */

		case stateHTTP_WAITING:
			iRV = xNetAccept(&sServHttpCtx, &sRR.sCtx, httpINTERVAL_MS) ;
			if (iRV < 0) {
				if (sServHttpCtx.error != EAGAIN) {
					HttpState = stateHTTP_DEINIT;
				}
				break ;
			}

			iRV = xNetSetRecvTO(&sRR.sCtx, httpINTERVAL_MS) ;
			if (iRV != erSUCCESS) {
				HttpState = stateHTTP_DEINIT ;
				break ;
			}
			xRtosSetStatus(flagHTTP_CLNT) ;			// mark as having a client connection
			HttpState = stateHTTP_CONNECTED ;
			IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), "connected\r\n") ;
			/* FALLTHRU */ /* no break */

		case stateHTTP_CONNECTED:
			iRV = xNetRecv(&sRR.sCtx, sRR.sUB.pBuf, sRR.sUB.Size) ;
			if (iRV > 0) {							// read something ?
				IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), "start parsing\r\n") ;
				if (sServHttpCtx.maxRx < iRV) {
					sServHttpCtx.maxRx = iRV;
				}
				http_parser 	sParser ;				// then process the packet
				http_parser_init(&sParser, HTTP_REQUEST) ;
				sParser.data		= &sRR ;
				// setup guidelines for parsing the request
				sRR.pVoid			= NULL ;
				sRR.hvContentLength	= 0UL ;
				sRR.hvDate			= 0 ;
				sRR.hvLastModified	= 0 ;
				sRR.hvStatusMess	= 0 ;
//				sRR.hvStatus		= 0 ;				// not needed, only in client session
				sRR.hvAccept		= ctUNDEFINED ;
				sRR.hvConnect		= coUNDEFINED ;
				sRR.hvContentType	= ctUNDEFINED ;
//				sRR.HdrField		= 0 ;
				sRR.f_allflags		= 0 ;
				sRR.f_parts			= 1 ;				// break URL up in parts
				sRR.f_query			= 1 ;				// break query up in parts
//				sRR.f_debug			= 1 ;				// enable debug output
				sRR.sUB.Used		= iRV ;
				iRV = xHttpCommonDoParsing(&sParser) ;
				IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), "Parsing done\r\n") ;
				if (iRV > 0) {							// build response if something was parsed....
					IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), "start response handler\r\n");
					iRV2 = xHttpServerResponseHandler(&sParser);
					IF_RP(debugTRACK && ioB1GET(ioHTTPtrack), "Tx done (%d)\r\n", iRV2) ;
				} else {
					iRV2 = erSUCCESS;
				}
				// socket closed, any error or coClose set
				if (iRV == 0 || iRV2 < erSUCCESS || sRR.sCtx.error != 0 || sRR.hvConnect == coClose) {
					vTaskHttpCloseClient();
				} else {
					// both parse & response handler & write was successful
				}
			} else if (iRV < erSUCCESS && sRR.sCtx.error != EAGAIN) {		// not EAGAIN/EWOULDBLOCK, socket closed OR real error
				vTaskHttpCloseClient();
			}
			break;
		default:
			SL_ERR(debugAPPL_PLACE) ;
		}
	}
	vTaskHttpDeInit();
	vRtosFree(sRR.sUB.pBuf) ;
	vRtosTaskDelete(NULL) ;
}

void vHttpStartStop(void) {
	if (ioB1GET(ioHTTPstart)) {
		xRtosClearStateRUN(taskHTTP_MASK);
		xRtosClearStateDELETE(taskHTTP_MASK);
		HttpHandle = xRtosTaskCreateStatic(vHttpTask, "http", httpSTACK_SIZE, NULL, httpPRIORITY, tsbHTTP, &ttsHTTP, tskNO_AFFINITY);
	} else {
		vRtosTaskTerminate(taskHTTP_MASK);
	}
}

void vHttpReport(void) {
	if (bRtosCheckStatus(flagHTTP_SERV) == 1) {
		xNetReport(&sServHttpCtx, "HTTPsrv", 0, 0, 0);
		printfx("\tFSM=%d  maxTX=%u  maxRX=%u  Rqst=0x%X\r\n", HttpState, sServHttpCtx.maxTx, sServHttpCtx.maxRx, fRqst) ;
	}
	if (bRtosCheckStatus(flagHTTP_CLNT) == 1) {
		xNetReport(&sRR.sCtx, "HTTPclt", 0, 0, 0);
	}
}
