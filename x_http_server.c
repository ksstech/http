/*
 * Copyright 2014-21 Andre M Maree / KSS Technologies (Pty) Ltd.
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
 */

/*
 * http_client.c
 */

#include	"FreeRTOS_Support.h"

#include 	"x_http_server.h"
#include 	"x_http_client.h"								// for xHttpFirmware????()
#include	"rules_parse_text.h"
#include	"task_control.h"

#include	"printfx.h"
#include	"syslog.h"
#include	"x_errors_events.h"
#include	"x_string_general.h"
#include	"x_string_to_values.h"
#include	"x_stdio.h"
#include	"x_time.h"
#include	"actuators.h"
#include	"commands.h"

#include	"hal_network.h"
#include	"hal_fota.h"
#include	"hal_storage.h"
#include	"hal_mcu.h"									// for halMCU_Restart()
#include	"hal_variables.h"

#include	<stdlib.h>
#include	<string.h>
#include	<limits.h>

// ############################### BUILD: debug configuration options ##############################

#define	debugFLAG					0xC000

#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ########################################### macros ##############################################

#define halNET_EXTEND_IP			0

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

static	const char	HtmlAPdetails[] =
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

static	const char	HtmlSTAdetails[] =
	"<html><head><title>'IRMACOS: STA details'</title></head><body>"
	"<h3>'IRMACOS: STA details'</h3>"
	"<p>Manufacturer    : " halDEV_VENDOR "</p>"
	"<p>Platform        : " halDEV_MODEL "</p>"
	"<p>Wifi FW version : " mySTRINGIFY(DEV_WIFI_VER) "</p>"
	"<p>Firmware Ver#   : " DEV_FW_VER_STR "</p>"
	"<form action='sta_update' method='get'><input type='submit' value='STA_Update'></form>"
	"</body></html>" ;

static	const char HtmlAPconfigOK[] =
	"<html><body><h3>IRMACOS: AP details set, rebooting now..</h3>"
	"<p>Please wait a minute and then confirm success</p>"
	"</body></html>" ;

static	const char HtmlAPconfigFAIL[] =
	"<html><body><h3>IRMACOS: AP details NOT set, try again..</h3>"
	"<p>Please make sure only using the allowed characters</p>"
	"</body></html>" ;

static	const char HtmlErrorNoHost[] =
	"<html><body><h2>No Host: header received</h2>"
	"<p>HTTP 1.1 requests must include the Host: header.</p>"
	"</body></html>" ;

static	const char HtmlErrorInvMethod[] =
	"<html><body><h2>Invalid method request</h2>"
	"<p>IRMACOS only support GET, not HEAD, POST, DELETE etc..</p>"
	"</body></html>" ;

static	const char HtmlErrorBadQuery[] =
	"<html><body><h2>Query key:value pair(s) mismatched</h2>"
	"</body></html>" ;

uint8_t			HttpState ;
netx_t			sServHttpCtx ;
http_rr_t	sRR = { 0 } ;

// ###################################### global variables #########################################


// ################################## local/static support functions ###############################

int32_t xHttpServerSetResponseStatus(http_parser * psParser, int32_t Status) {
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
	if (INRANGE(HTTP_STATUS_BAD_REQUEST, Status, HTTP_STATUS_UNAVAILABLE_FOR_LEGAL_REASONS, int32_t)) {
		psRR->hvConnect = coClose ;					// force connection to be closed
	}
	return erSUCCESS ;
}

int32_t	xHttpSendResponse(http_parser * psParser, const char * format, ...) {
	http_rr_t * psRR = psParser->data ;
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psParser) && halCONFIG_inSRAM(psRR) && halCONFIG_inSRAM(psRR->sUB.pBuf)) ;
	int32_t iRV ;
	iRV = socprintfx(&psRR->sCtx, "HTTP/1.1 %d %s\r\n", psParser->status_code, psRR->pcStatMes) ;
	iRV += socprintfx(&psRR->sCtx, "Date: %#Z\r\n", &sTSZ) ;
	iRV += socprintfx(&psRR->sCtx, "Content-Language: en-US\r\n") ;
	if (psRR->hvConnect) {
		iRV += socprintfx(&psRR->sCtx, "Connection: %s\r\n", coValues[psRR->hvConnect]) ;
		if (psRR->hvConnect == coKeepAlive) {
			iRV += socprintfx(&psRR->sCtx, "Keep-Alive: timeout=3\r\n") ;
		}
	}
	if (psRR->hvContentType) {
		iRV += socprintfx(&psRR->sCtx, "Content-Type: %s\r\n", ctValues[psRR->hvContentType]) ;
	}

	va_list vArgs ;
	va_start(vArgs, format) ;
	/* The next line does not really print anything. By specifying the NULL as the string
	 * pointer it performs a test print and simply return the number of characters that
	 * would have been printed if a destination was specified.*/
	psRR->hvContentLength = vsprintfx(NULL, format, vArgs) ;
	/* Now do the actual formatted print of the content length with an additional 2 chars
	 * added for the extra CR + LF pair to form the blank line after the header values */
	iRV += socprintfx(&psRR->sCtx, "Content-Length: %d\r\n\r\n", psRR->hvContentLength + 2) ;
	iRV += vsocprintfx(&psRR->sCtx, format, vArgs) ;				// add the actual content
	va_end(vArgs) ;
	iRV += socprintfx(&psRR->sCtx, "\r\n") ;						// add the final CR+LF after the body

	IF_PRINT(debugTRACK && psRR->f_debug, "Content:\n%.*s", psRR->sUB.Used, psRR->sUB.pBuf) ;
	return iRV ;
}

int32_t	xHttpServerParseString(char * pVal, char * pDst) {
	if (xStringParseEncoded(pVal, pDst) == erFAILURE) {
		return erFAILURE ;
	}
	IF_PRINT(debugTRACK, "%s->%s\n", pVal, pDst) ;
	return erSUCCESS ;
}

int32_t	xHttpServerParseIPaddress(char * pSrc, uint32_t * pDst) {
	if (xStringParseEncoded(pSrc, NULL) == erFAILURE) {
		return erFAILURE ;
	}
	IF_PRINT(debugTRACK, "%s->%s", pSrc) ;
	if (pcStringParseIpAddr(pSrc, (px_t) pDst) == pcFAILURE) {
		*pDst = 0 ;
		return erFAILURE ;
	}
	IF_PRINT(debugTRACK, " : %-I\n", *pDst) ;
	return erSUCCESS ;
}

// ######################################## URL handlers ###########################################

int32_t	xHttpHandle_API(http_parser * psParser) {
	static const char format[] = "<html><body><h2>Function result</h2><pre>%.*s</pre></body></html>" ;
	http_rr_t * psRR = psParser->data ;
	int32_t iRV ;
	for (int32_t i = 1; i < httpYUAREL_MAX_PARTS && psRR->parts[i] != NULL; ++i) {
		char * pcCommand = psRR->parts[i] ;
		xStringParseEncoded(pcCommand, pcCommand) ;
		IF_PRINT(debugTRACK, "#%d = '%s'\n", i, pcCommand) ;
		while (*pcCommand != CHR_NUL) {
			vCommandInterpret((int) *pcCommand, 0) ;
			++pcCommand ;
		}
		if (pcCommand - psRR->parts[i] > 1) {
			vCommandInterpret((int) CHR_CR, 0) ;
		}
	}
	if (xUBufAvail(&sRTCvars.sRTCbuf) > 0) {
		/* Definitive problem here if the volume of output from vCommandInterpret() exceed the
		 * size of the buffer. In this case some content would have been overwritten and the
		 * pointers would have wrapped and could point somewhere other than the start of the
		 * buffer. In this case, the response as sent will be incomplete or invalid.
		 * XXX change handling to accommodate sending 2 separate blocks
		 */
		iRV = xHttpSendResponse(psParser, format, xUBufAvail(&sRTCvars.sRTCbuf), pcUBufTellRead(&sRTCvars.sRTCbuf)) ;
		vUBufReset(&sRTCvars.sRTCbuf) ;
	} else {
		iRV = xHttpSendResponse(psParser, "<html><body><h2>Command completed</h2></body></html>") ;
	}
	return iRV ;
}

// ################################### Common HTTP API functions ###################################

void	vHttpServerCloseClient(netx_t * psCtx) {
	xRtosClearStatus(flagHTTP_CLNT) ;
	HttpState = stateHTTP_WAITING ;
	xNetClose(psCtx) ;
	IF_CTRACK(debugTRACK, "closing\n") ;
}

/**
 * xHttpResponseHandler() - called after header (and content) parsing has completed
 * @comment Needs to use info from the parsing stage to build the response
 * @param psParser
 * @return	size of the response created (bytes)
 */
int32_t	xHttpServerResponseHandler(http_parser * psParser) {
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psParser) && halCONFIG_inSRAM(psParser->data)) ;
	http_rr_t * psRR = psParser->data ;
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psRR->sUB.pBuf)) ;

	int32_t	iURL = -1, iRV, i ;
	if (psParser->http_errno) {
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_NOT_ACCEPTABLE) ;
		psRR->pcBody	= (char *) http_errno_description(HTTP_PARSER_ERRNO(psParser)) ;
		SL_ERR("%s (%s)", psRR->pcBody, http_errno_name(HTTP_PARSER_ERRNO(psParser))) ;
		psRR->hvContentType = ctTextPlain ;

	} else if (psParser->method != HTTP_GET) {			// Invalid METHOD
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_NOT_IMPLEMENTED) ;
		psRR->pcBody	= (char *) HtmlErrorInvMethod ;
		SL_ERR("Method not supported (%d)", psParser->method) ;

	} else if (psRR->f_host == 0) {						// host not provided
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_BAD_REQUEST) ;
		psRR->pcBody	= (char *) HtmlErrorNoHost ;
		SL_ERR("Host name/IP not provided") ;

	} else {		// at this stage all parsing results are OK, just the URL to be matched and processed.
		if (*psRR->url.path == CHR_NUL) {				// STEP1: start by matching the URL
			iURL = urlROOT ;							// do NOT try to match, lost single '/'
		} else {
			iURL = xHttpCommonFindMatch(UrlTable, NO_MEM(UrlTable), psRR->url.path, xstrlen(psRR->url.path)) ;
			if (iURL == 0)	{
				iURL = urlNOTFOUND ;
			}
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
#if		(halNET_EXTEND_IP == 1)
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
			iRV = xHttpServerParseString(psRR->params[i++].val, (char *) tmpWifi.ssid) ;
			if (iRV == erSUCCESS) {
				iRV = xHttpServerParseString(psRR->params[i++].val, (char *) tmpWifi.pswd) ;
			}
#if		(halNET_EXTEND_IP == 1)
			if (iRV == erSUCCESS)
				iRV = xHttpServerParseIPaddress(psRR->params[i++].val, &tmpWifi.ipNM) ;
			if (iRV == erSUCCESS)
				iRV = xHttpServerParseIPaddress(psRR->params[i++].val, &tmpWifi.ipGW) ;
			if (iRV == erSUCCESS)
				iRV = xHttpServerParseIPaddress(psRR->params[i++].val, &tmpWifi.ipSTA) ;
			if (iRV == erSUCCESS)
				iRV = xHttpServerParseIPaddress(psRR->params[i++].val, &tmpWifi.ipDNS1) ;
			if (iRV == erSUCCESS)
				iRV = xHttpServerParseIPaddress(psRR->params[i++].val, &tmpWifi.ipDNS2) ;
#endif
			if (iRV == erSUCCESS) {						// last parameter in the list
				iRV = xHttpServerParseIPaddress(psRR->params[i++].val, &tmpWifi.ipMQTT) ;
			}
			// Check if values allow successful Wifi connection (& persist if successful)
			iRV = halWL_TestCredentials((char *) tmpWifi.ssid, (char *) tmpWifi.pswd) ;
			if (iRV == erSUCCESS) {						// inform client of success or not....
				xHttpServerSetResponseStatus(psParser, HTTP_STATUS_OK) ;
				psRR->pcBody	= (char *) HtmlAPconfigOK ;
				xRtosSetStatus(flagAPP_RESTART) ;
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
		iRV = xHttpSendResponse(psParser, psRR->pcBody) ;
		IF_PRINT(debugTRACK, "Response sent iRV=%d\n", iRV) ;
	}
	if (sServHttpCtx.maxTx < iRV) {
		sServHttpCtx.maxTx = iRV ;
	}

	return iRV ;
}

/**
 *vTaskHttp()
 * @param pvParameters
 *
 * Responsibilities:
 * 	Serve HTML to capture SSID & PSWD from client
 * 	Respond to /restart (as emergency)
 */
void	vTaskHttp(void * pvParameters) {
	IF_TRACK(debugAPPL_THREADS, debugAPPL_MESS_UP) ;
	vTaskSetThreadLocalStoragePointer(NULL, 1, (void *)taskHTTP) ;
	sRR.sUB.pBuf	= malloc(sRR.sUB.Size = httpSERVER_BUFSIZE) ;
	HttpState 		= stateHTTP_INIT ;
	xRtosSetStateRUN(taskHTTP) ;

	while (bRtosVerifyState(taskHTTP)) {
		xRtosWaitStatusANY(flagL3_ANY, portMAX_DELAY) ;	// ensure IP is up and running...
		switch(HttpState) {
		int32_t	iRV ;
		case stateHTTP_DEINIT:
			IF_CTRACK(debugTRACK, "de-init\n") ;
			xRtosClearStatus(flagHTTP_SERV | flagHTTP_CLNT) ;
			xNetClose(&sRR.sCtx) ;
			xNetClose(&sServHttpCtx) ;
			HttpState = stateHTTP_INIT ;
			break ;					// must NOT fall through since the Lx status might have changed

		case stateHTTP_INIT:
			IF_CTRACK(debugTRACK, "init\n") ;
			memset(&sServHttpCtx, 0 , sizeof(sServHttpCtx)) ;
			sServHttpCtx.sa_in.sin_family	= AF_INET ;
			sServHttpCtx.type				= SOCK_STREAM ;
			sServHttpCtx.sa_in.sin_port		= htons(IP_PORT_HTTP) ;
			sServHttpCtx.flags				|= SO_REUSEADDR ;
			iRV = xNetOpen(&sServHttpCtx) ;
			if (iRV < erSUCCESS) {
				HttpState = stateHTTP_DEINIT ;
				break ;
			}
			xRtosSetStatus(flagHTTP_SERV) ;
			HttpState = stateHTTP_WAITING ;
			IF_CTRACK(debugTRACK, "waiting\n") ;
			/* FALLTHRU */ /* no break */

		case stateHTTP_WAITING:
			iRV = xNetAccept(&sServHttpCtx, &sRR.sCtx, httpINTERVAL_MS) ;
			if (iRV < 0) {
				if (sServHttpCtx.error != EAGAIN) {
					HttpState = stateHTTP_DEINIT ;
				}
				break ;
			}

			iRV = xNetSetRecvTimeOut(&sRR.sCtx, httpINTERVAL_MS) ;
			if (iRV != erSUCCESS) {
				HttpState = stateHTTP_DEINIT ;
				break ;
			}
			xRtosSetStatus(flagHTTP_CLNT) ;			// mark as having a client connection
			HttpState = stateHTTP_CONNECTED ;
			IF_CTRACK(debugTRACK, "connected\n") ;
			/* FALLTHRU */ /* no break */

		case stateHTTP_CONNECTED:
			iRV = xNetRead(&sRR.sCtx, sRR.sUB.pBuf, sRR.sUB.Size) ;
			if (iRV > 0) {							// read something ?
				IF_CTRACK(debugTRACK, "start parsing\n") ;
				if (sServHttpCtx.maxRx < iRV) {		// yes, update the Rx packet stats
					sServHttpCtx.maxRx = iRV ;
				}
				http_parser 	sParser ;				// then process the packet
				http_parser_init(&sParser, HTTP_REQUEST) ;
				sParser.data		= &sRR ;
				// setup guidelines for parsing the request
				sRR.pVoid		= NULL ;
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
				if (iRV > 0) {							// build response if something was parsed....
					IF_CTRACK(debugTRACK, "start response handler\n") ;
					xStdioBufLock(portMAX_DELAY) ;
					iRV = xHttpServerResponseHandler(&sParser) ;
					xStdioBufUnLock() ;
				}
				IF_CTRACK(debugTRACK, "Parsing done\n") ;
				// socket closed or error occurred or coClose was set, close the connection
				if (iRV == 0 || 						// nothing parsed or socket closed?
					sRR.sCtx.error != 0 || 				// any error (even EAGAIN) on write ?
					sRR.hvConnect == coClose) {			// or connection must be closed ?
					vHttpServerCloseClient(&sRR.sCtx) ;	// then close the damn thing
					IF_CTRACK(debugTRACK, "client closed\n") ;
				} else {
					// both parse & response handler & write was successful
				}
			} else if (sRR.sCtx.error != EAGAIN) {		// not EAGAIN/EWOULDBLOCK, socket closed OR real error
				vHttpServerCloseClient(&sRR.sCtx) ;
				IF_CTRACK(debugTRACK, "client closed\n") ;
			}
			IF_CTRACK(debugTRACK, "Tx done\n") ;
			break ;

		default:	SL_ERR(debugAPPL_PLACE) ;
		}
		vTaskDelay(pdMS_TO_TICKS(httpINTERVAL_MS)) ;
	}
	xRtosClearStatus(flagHTTP_SERV) ;
	vPortFree(sRR.sUB.pBuf) ;
	xNetClose(&sServHttpCtx) ;
	xNetClose(&sRR.sCtx) ;
	IF_TRACK(debugAPPL_THREADS, debugAPPL_MESS_DN) ;
	vTaskDelete(NULL) ;
}

void	vTaskHttpInit(void) {
	xRtosTaskCreate(vTaskHttp, "HTTP", httpSTACK_SIZE, NULL, httpPRIORITY, NULL, tskNO_AFFINITY) ;
}

void	vHttpReport(void) {
	if (bRtosCheckStatus(flagHTTP_SERV) == 1) {
		xNetReport(&sServHttpCtx, "HTTPsrv", 0, 0, 0) ;
		printfx("\tFSM=%d  maxTX=%u  maxRX=%u\n", HttpState, sServHttpCtx.maxTx, sServHttpCtx.maxRx) ;
	}
	if (bRtosCheckStatus(flagHTTP_CLNT) == 1) {
		xNetReport(&sRR.sCtx, "HTTPclt", 0, 0, 0) ;
	}
}
