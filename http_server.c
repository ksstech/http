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

#include 	"http_server.h"
#include 	"http_client.h"								// for xHttpFirmware????()
#include	"rules_parse_text.h"
#include	"FreeRTOS_Support.h"
#include	"task_console.h"

#include	"x_debug.h"
#include	"x_retarget.h"
#include	"x_syslog.h"
#include	"x_errors_events.h"
#include	"x_string_to_values.h"
#include	"x_string_general.h"
#include	"x_time.h"
#include	"x_string_to_values.h"
#include	"actuators.h"

#include	"hal_network.h"
#include	"hal_fota.h"
#include	"hal_storage.h"
#include	"hal_mcu.h"									// for halMCU_Restart()

#include	<stdlib.h>
#include	<string.h>
#include	<limits.h>

// ############################### BUILD: debug configuration options ##############################

#define	debugFLAG						(0x0004)
#define	debugTRACK						(debugFLAG & 0x0001)
#define	debugBUILD						(debugFLAG & 0x0002)
#define	debugPARAM						(debugFLAG & 0x0004)

// ########################################### macros ##############################################


// ###################################### local ie static variables ################################

uint8_t		HttpState ;
sock_ctx_t	sServHttpCtx ;

static	const char	HtmlAPdetails[] =
	"<html><head><title>'IRMACOS: AP details'</title></head><body><h3>'IRMACOS: AP details'</h3>"
	"<p>AP SSID: Maximum 32 characters including A-Z, a-z and 0-9</p>"
	"<p>AP PSWD: Same rules as SSID but max 64 characters length</p>"
	"<form action='save_ap' method='get'>AP SSID:<br><input type='text' name='ssid' size='32'><br>AP PSWD:<br>"
	"<input type='text' name='pswd' size='64'><br><br><input type='submit' value='Submit'></form></body></html>" ;

static	const char	HtmlSTAdetails[] =
	"<html><head><title>'IRMACOS: STA details'</title></head><body>"
	"<h3>'IRMACOS: STA details'</h3>"
	"<p>Manufacturer    : " DEV_VENDOR "</p>"
	"<p>Platform        : " DEV_PLATFORM "</p>"
	"<p>Wifi FW version : " mySTRINGIFY(DEV_WIFI_VER) "</p>"
	"<p>Firmware Ver#   : " DEV_FW_VER_STR "</p>"
	"<form action='sta_update' method='get'><input type='submit' value='STA_Update'></form></body></html>" ;

static	const char HtmlAPconfigOK[] =
	"<html><body><h3>IRMACOS: AP details set, rebooting now..</h3>"
	"<p>Please wait a minute and then confirm success</p></body></html>" ;

static	const char HtmlAPconfigFAIL[] =
	"<html><body><h3>IRMACOS: AP details NOT set, try again..</h3>"
	"<p>Please make sure only using the allowed characters</p></body></html>" ;

static	const char HtmlErrorNoHost[] =
	"<html><body><h2>No Host: header received</h2>"
	"<p>HTTP 1.1 requests must include the Host: header.</p></body></html>" ;

static	const char HtmlErrorInvMethod[] =
	"<html><body><h2>Invalid method request</h2>"
	"<p>IRMACOS only support GET, not HEAD, POST, DELETE etc..</p></body></html>" ;

static	const char HtmlErrorBadQuery[] =
	"<html><body><h2>Query key:value pair(s) mismatched</h2></body></html>" ;

// ###################################### global variables #########################################

http_reqres_t	sRR = { 0 } ;

// ################################## local/static support functions ###############################

int32_t	xHttpServerParseEncoded(char * pWrite) {
	char * pRead	= pWrite ;
	int32_t iRetVal = 0 ;
//	PRINT("%s  ", pWrite) ;
	while(*pRead != 0) {
		if (*pRead == CHR_PERCENT) {					// escape char?
			int32_t Val1 = xHexCharToValue(*++pRead) ;
			if (Val1 == erFAILURE) {
				return erFAILURE ;
			}
			int32_t Val2 = xHexCharToValue(*++pRead) ;
			if (Val2 == erFAILURE) {
				return erFAILURE ;
			}
			IF_PRINT(debugBUILD, "[%d+%d=%d]  ", Val1, Val2, (Val1 << 4) + Val2) ;
			*pWrite++ = (Val1 << 4) + Val2 ;
			++pRead ;									// no, skip over source
		} else {
			*pWrite++ = *pRead++ ;						// copy as is to (new) position
		}
		++iRetVal ;										// & adjust count...
	}
	*pWrite = CHR_NUL ;								// terminate
	IF_PRINT(debugBUILD, "%s\n", pWrite-iRetVal) ;
	return iRetVal ;
}

int32_t xHttpServerSetResponseStatus(http_parser * psParser, int32_t Status) {
	http_reqres_t * psRR	= psParser->data ;
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
	default:							myASSERT(0) ;								break ;
	}
	if (INRANGE(HTTP_STATUS_BAD_REQUEST, Status, HTTP_STATUS_UNAVAILABLE_FOR_LEGAL_REASONS, int32_t)) {
		psRR->hvConnect = coClose ;					// force connection to be closed
	}
	return erSUCCESS ;
}

void	vHttpBuildResponse(http_parser * psParser, const char * format, ...) {
	http_reqres_t * psRR = psParser->data ;
	IF_myASSERT(debugPARAM, INRANGE_SRAM(psParser) && INRANGE_SRAM(psRR) && INRANGE_SRAM(psRR->sBuf.pBuf)) ;

	socprintf(&psRR->sCtx, "HTTP/1.1 %d %s\r\n", psParser->status_code, psRR->pcStatMes) ;
	socprintf(&psRR->sCtx, "Date: %#Z\r\n", &sTSZ) ;
	socprintf(&psRR->sCtx, "Content-Language: en-US\r\n") ;
	if (psRR->hvConnect) {
		socprintf(&psRR->sCtx, "Connection: %s\r\n", coValues[psRR->hvConnect]) ;
		if (psRR->hvConnect == coKeepAlive) {
			socprintf(&psRR->sCtx, "Keep-Alive: timeout=3\r\n") ;
		}
	}
	if (psRR->hvContentType) {
		socprintf(&psRR->sCtx, "Content-Type: %s\r\n", ctValues[psRR->hvContentType]) ;
	}

	va_list vArgs ;
	va_start(vArgs, format) ;
	/* The next line does not really print anything. By specifying the NULL as the string
	 * pointer it performs a test print and simply return the number of characters that
	 * would have been printed if a destination was specified.*/
	psRR->hvContentLength = xvsprintf(NULL, format, vArgs) ;
	/* Now do the actual formatted print of the content length with an additional 2 chars
	 * added for the extra CR + LF pair to form the blank line after the header values */
	socprintf(&psRR->sCtx, "Content-Length: %d\r\n\r\n", psRR->hvContentLength + 2) ;
	vsocprintf(&psRR->sCtx, format, vArgs) ;				// add the actual content
	va_end(vArgs) ;
	socprintf(&psRR->sCtx, "\r\n") ;						// add the final CR+LF after the body

#if	defined(DEBUG)
	if (psRR->f_debug) {
		PRINT("Content:\n%.*s", psRR->sBuf.Used, psRR->sBuf.pBuf) ;
	}
#endif
	return ;
}

// ######################################## URL handlers ###########################################

enum {
	urlROOT,
	urlSAVE_AP,
	urlAPI,
	urlNOTFOUND,
	urlNUMBER,							// Insert ALL URL endpoints above here
} ;

static	const char * UrlTable[] = {
	[urlROOT]		= "/",
	[urlSAVE_AP]	= "save_ap",
	[urlAPI]		= "api",
} ;

void vHttpHandle_API(http_parser * psParser) {
	static const char format[] = "<html><body><h2>Function result</h2><pre>%.*s</pre></body></html>" ;
	http_reqres_t * psRR = psParser->data ;
	vCommandInterpret(1, (int) *psRR->parts[1]) ;
	ubuf_t * psBuf = &sBufStdOut ;
	if (psBuf->Used) {
		vHttpBuildResponse(psParser, format, psBuf->Used, psBuf->pBuf + psBuf->IdxRD) ;
		psBuf->Used	= psBuf->IdxWR = psBuf->IdxRD = 0 ;	// reset pointers to reflect empty
	} else {
		vHttpBuildResponse(psParser, "<html><body><h2>Command completed</h2></body></html>") ;
	}
}

// ################################### Common HTTP API functions ###################################

void	vHttpServerCloseClient(sock_ctx_t * psCtx) {
	vRtosClearStatus(flagNET_L5_HTTP_CLNT) ;
	HttpState = stateHTTP_WAITING ;
	if (sRR.sCtx.maxTx < psCtx->maxTx) {
		sRR.sCtx.maxTx = psCtx->maxTx ;
	}
	if (sRR.sCtx.maxRx < psCtx->maxRx) {
		sRR.sCtx.maxRx = psCtx->maxRx ;
	}
	xNetClose(psCtx) ;
	IF_SL_DBG(debugTRACK, "closing") ;
}

/**
 * xHttpResponseHandler() - called after header (and content) parsing has completed
 * @comment Needs to use info from the parsing stage to build the response
 * @param psParser
 * @return	size of the response created (bytes)
 */
int32_t	xHttpServerResponseHandler(http_parser * psParser) {
	IF_myASSERT(debugPARAM, INRANGE_SRAM(psParser) && INRANGE_SRAM(psParser->data)) ;
	http_reqres_t * psRR = psParser->data ;
	IF_myASSERT(debugPARAM, INRANGE_SRAM(psRR->sBuf.pBuf)) ;

	int32_t	iURL = -1 ;
	if (psParser->http_errno) {
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_NOT_ACCEPTABLE) ;
		psRR->pcBody	= (char *) http_errno_description(HTTP_PARSER_ERRNO(psParser)) ;
		psRR->hvContentType = ctTextPlain ;
		SL_ERR("%s (%s)", psRR->pcBody, http_errno_name(HTTP_PARSER_ERRNO(psParser))) ;

	} else if (psParser->method != HTTP_GET) {							// Invalid METHOD
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_NOT_IMPLEMENTED) ;
		psRR->pcBody	= (char *) HtmlErrorInvMethod ;
		SL_ERR("Method not supported (%d)", psParser->method) ;

	} else if (psRR->f_host == 0) {									// host not provided
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_BAD_REQUEST) ;
		psRR->pcBody	= (char *) HtmlErrorNoHost ;

	} else {		// at this stage all parsing results are OK, just the URL to be matched and processed.
		if (*psRR->url.path == CHR_NUL) {				// STEP1: start by matching the URL
			iURL = urlROOT ;							// do NOT try to match, lost single '/'
		} else {
			iURL = xHttpCommonFindMatch(UrlTable, NUM_OF_MEMBERS(UrlTable), psRR->url.path, strlen(psRR->url.path)) ;
			if (iURL == 0)	{ iURL = urlNOTFOUND ; }
		}
	}

	switch(iURL) {
	case urlROOT:
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_OK) ;
		psRR->pcBody = (wifi_mode == WIFI_MODE_AP) ? (char *) HtmlAPdetails : (char *) HtmlSTAdetails ;
		break ;
	case urlSAVE_AP:
		if ((strcmp(psRR->params[0].key, halSTORAGE_KEY_SSID) != 0) ||
			(strcmp(psRR->params[1].key, halSTORAGE_KEY_PSWD) != 0)) {
			xHttpServerSetResponseStatus(psParser, HTTP_STATUS_BAD_REQUEST) ;
			psRR->pcBody	= (char *) HtmlErrorBadQuery ;

		} else {
			int32_t	iRetVal = xHttpServerParseEncoded(psRR->params[0].val) ;
			if (iRetVal > 0) {
				iRetVal = halSTORAGE_WriteKeyValue(halSTORAGE_STORE, psRR->params[0].key, (x32_t) psRR->params[0].val, vfSXX) ;
			}
			if (iRetVal == erSUCCESS) {
				iRetVal = xHttpServerParseEncoded(psRR->params[1].val) ;
				if (iRetVal > 0) {
					iRetVal = halSTORAGE_WriteKeyValue(halSTORAGE_STORE, psRR->params[1].key, (x32_t) psRR->params[1].val, vfSXX) ;
				}
			}
			if (iRetVal == erSUCCESS) {
				xHttpServerSetResponseStatus(psParser, HTTP_STATUS_OK) ;
				psRR->pcBody	= (char *) HtmlAPconfigOK ;
				vRtosSetStatus(flagAPP_RESTART) ;
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
			psRR->hdlr_rsp	= vHttpHandle_API ;
		} else {
			xHttpServerSetResponseStatus(psParser, HTTP_STATUS_BAD_REQUEST) ;
			psRR->pcBody 	= "<html><body><h2>** API command option Required **</h2></body></html>" ;
		}
		break ;
//	case urlNOTFOUND:
	default:
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_NOT_FOUND) ;
		psRR->pcBody 	= "<html><body><h2>** URL Nor found **</h2></body></html>" ;
		break ;
	}

	if (psRR->f_bodyCB && psRR->hdlr_rsp) {
		psRR->hdlr_rsp(psParser) ;						// Add dynamic content to buffer via callback
	} else {
		vHttpBuildResponse(psParser, psRR->pcBody) ;
	}
	return erSUCCESS ;
}

/**
 *vHttpServerTask()
 * @param pvParameters
 *
 * Responsibilities:
 * 1. Scheduled checks for FW upgrades
 * 2. Initial Priority 1 FOTA
 * 3. GeoLocation, alternative to host
 * 4. GeoTimeZone, alternative to host
 * 5. Rules download, test & Plan B only
 * 6. IDENT download, test & Plan B only (To be done)
 * 7. Serve HTML to capture SSID & PSWD from client
 * 8. Respond to /restart (as emergency)
 */
void	vHttpServerTask(void * pvParameters) {
	IF_TRACK_PRINT(debugAPPL_THREADS, messageTASK_START) ;
	sRR.sBuf.pBuf	= pvPortMalloc(sRR.sBuf.Size = httpSERVER_BUFSIZE) ;
	HttpState 		= stateHTTP_INIT ;

	while (xRtosVerifyState(taskHTTP)) {
		vRtosWaitStatus(flagNET_L3_UP) ;				// ensure IP is up and running...
		switch(HttpState) {
		int32_t	iRetVal ;
		case stateHTTP_RESET:
			IF_SL_DBG(debugTRACK, "reset") ;
			vRtosClearStatus(flagNET_L5_HTTP_SERV | flagNET_L5_HTTP_CLNT) ;
			xNetClose(&sRR.sCtx) ;
			xNetClose(&sServHttpCtx) ;
			HttpState = stateHTTP_INIT ;
			/* no break */

		case stateHTTP_INIT:
			IF_SL_DBG(debugTRACK, "init") ;
			memset(&sServHttpCtx, 0 , sizeof(sock_ctx_t)) ;
			sServHttpCtx.sa_in.sin_family		= AF_INET ;
			sServHttpCtx.type					= SOCK_STREAM ;
//			sServHttpCtx.psSec					= ? ;
#if 0
			sServHttpCtx.d_data					= 1 ;
			sServHttpCtx.d_read					= 1 ;
			sServHttpCtx.d_write				= 1 ;
#endif
			sServHttpCtx.sa_in.sin_port			= htons(sServHttpCtx.psSec ? IP_PORT_HTTPS : IP_PORT_HTTP) ;
			iRetVal = xNetOpen(&sServHttpCtx) ;
			if (iRetVal < erSUCCESS) {
				HttpState = stateHTTP_RESET ;
				break ;
			}
			vRtosSetStatus(flagNET_L5_HTTP_SERV) ;
			HttpState = stateHTTP_WAITING ;
			IF_SL_DBG(debugTRACK, "waiting") ;
			/* no break */

		case stateHTTP_WAITING:
			iRetVal = xNetAccept(&sServHttpCtx, &sRR.sCtx, httpINTERVAL_MS) ;
			if (iRetVal < 0) {
				if (sServHttpCtx.error != EAGAIN) {
					HttpState = stateHTTP_RESET ;
				}
				break ;
			}

			iRetVal = xNetSetRecvTimeOut(&sRR.sCtx, httpINTERVAL_MS) ;
			if (iRetVal != erSUCCESS) {
				HttpState = stateHTTP_RESET ;
				break ;
			}
			vRtosSetStatus(flagNET_L5_HTTP_CLNT) ;		// mark as having a client connection
			HttpState = stateHTTP_CONNECTED ;
			IF_SL_DBG(debugTRACK, "connected") ;
			/* no break */

		case stateHTTP_CONNECTED:
			iRetVal = xNetRead(&sRR.sCtx, sRR.sBuf.pBuf, sRR.sBuf.Size) ;
			if (iRetVal > 0) {
				http_parser 	sParser ;
				http_parser_init(&sParser, HTTP_REQUEST) ;
				sParser.data		= &sRR ;
				// setup guidelines for parsing the request
				sRR.hdlr_rsp		= 0 ;
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

				sRR.sBuf.Used = iRetVal ;
				iRetVal = xHttpCommonDoParsing(&sParser) ;
				if (iRetVal > 0) {						// build response if something was parsed....
					xStdOutLock(portMAX_DELAY) ;
					xHttpServerResponseHandler(&sParser) ;
					iRetVal = xNetWrite(&sRR.sCtx, sRR.sBuf.pBuf, sRR.sBuf.Used) ;
					xStdOutUnLock() ;
				}
				// socket closed or error occurred or coClose was set, close the connection
				if (iRetVal == 0 || 					// nothing parsed or socket closed?
					sRR.sCtx.error != 0 || 				// any error (even EAGAIN) on write ?
					sRR.hvConnect == coClose) {			// or connection must be closed ?
					vHttpServerCloseClient(&sRR.sCtx) ;	// then close the damn thing
				} else {
					// both parse & response handler & write was successful
				}
			} else if (sRR.sCtx.error != EAGAIN) {		// not EAGAIN/EWOULDBLOCK, socket closed OR real error
				vHttpServerCloseClient(&sRR.sCtx) ;
			}
			break ;

		default:
			myASSERT(0) ;
			break ;
		}
	}
	vPortFree(sRR.sBuf.pBuf) ;
	xNetClose(&sServHttpCtx) ;
	xNetClose(&sRR.sCtx) ;
	IF_TRACK_PRINT(debugAPPL_THREADS, messageTASK_DELETE) ;
	vTaskDelete(NULL) ;
}

void	vHttpServerTaskInit(void) { xRtosTaskCreate(vHttpServerTask, "HTTP", httpSTACK_SIZE, NULL, 6, NULL, INT_MAX) ; }

void	vHttpReport(int32_t Handle) {
	if (xRtosCheckStatus(flagNET_L5_HTTP_CLNT)) {
		xNetReport(Handle, &sRR.sCtx, __FUNCTION__, 0, 0, 0) ;
	}
	if (xRtosCheckStatus(flagNET_L5_HTTP_SERV)) {
		xNetReport(Handle, &sServHttpCtx, __FUNCTION__, 0, 0, 0) ;
		xdprintf(Handle, "\t\tState=%d  maxTX=%u  maxRX=%u\n", HttpState, sRR.sCtx.maxTx, sRR.sCtx.maxRx) ;
	}
}
