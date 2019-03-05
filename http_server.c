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
#include	"task_control.h"

#include	"x_debug.h"
#include	"x_retarget.h"
#include	"x_syslog.h"
#include	"x_errors_events.h"
#include	"x_string_general.h"
#include	"x_string_to_values.h"
#include	"x_time.h"
#include	"actuators.h"

#include	"hal_network.h"
#include	"hal_fota.h"
#include	"hal_storage.h"
#include	"hal_mcu.h"									// for halMCU_Restart()

#include	<stdlib.h>
#include	<string.h>
#include	<limits.h>

// ############################### BUILD: debug configuration options ##############################

#define	debugFLAG						0xC000

#define	debugTRACK						(debugFLAG & 0x0001)
#define	debugBUILD						(debugFLAG & 0x0002)

#define	debugPARAM						(debugFLAG & 0x4000)
#define	debugRESULT						(debugFLAG & 0x8000)

// ########################################### macros ##############################################


// ###################################### enumerations #############################################

enum {
	urlROOT,
	urlSAVE_AP,
	urlAPI,
	urlNOTFOUND,
	urlNUMBER,							// Insert ALL URL endpoints above here
} ;

// ###################################### local ie static variables ################################

static	const char * UrlTable[] = {
	[urlROOT]		= "/",
	[urlSAVE_AP]	= "save_ap",
	[urlAPI]		= "api",
} ;

static	const char	HtmlAPdetails[] =
	"<html><head><title>'IRMACOS: AP details'</title></head>"
	"<body><h3>'IRMACOS: AP details'</h3>"
	"<p>AP SSID: Maximum 32 characters including A-Z, a-z and 0-9</p>"
	"<p>AP PSWD: Same rules as SSID but max 64 characters length</p>"
#if		(halNET_BUILD_STATIC == 1)
	"<p>IP for network, gateway, station and DNS server(s)</p>"
	"<p> specify without spaces or leading 0's</p>"
#endif
	"<form action='save_ap' method='get'>"
	"AP SSID:<br><input type='text' name='ssid' size='32'><br>"
	"AP PSWD:<br><input type='text' name='pswd' size='64'><br>"
#if		(halNET_BUILD_STATIC == 1)
	"IP NetMask:<br><input type='text' name='nm' size='16'><br>"
	"IP Gateway:<br><input type='text' name='gw' size='16'><br>"
	"IP Address:<br><input type='text' name='ip' size='16'><br>"
	"IP DNS #1 :<br><input type='text' name='d1' size='16'><br>"
	"IP DNS #2 :<br><input type='text' name='d2' size='16'><br>"
#endif
	"<br><input type='submit' value='Submit'>"
	"</form></body></html>" ;

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

uint8_t			HttpState ;
sock_ctx_t		sServHttpCtx ;
http_reqres_t	sRR = { 0 } ;

// ###################################### global variables #########################################


// ################################## local/static support functions ###############################

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
	default:							SL_ERR(debugAPPL_PLACE) ;
	}
	if (INRANGE(HTTP_STATUS_BAD_REQUEST, Status, HTTP_STATUS_UNAVAILABLE_FOR_LEGAL_REASONS, int32_t)) {
		psRR->hvConnect = coClose ;					// force connection to be closed
	}
	return erSUCCESS ;
}

int32_t	xHttpSendResponse(http_parser * psParser, const char * format, ...) {
	http_reqres_t * psRR = psParser->data ;
	IF_myASSERT(debugPARAM, INRANGE_SRAM(psParser) && INRANGE_SRAM(psRR) && INRANGE_SRAM(psRR->sBuf.pBuf)) ;
	int32_t iRV ;
	iRV = socprintf(&psRR->sCtx, "HTTP/1.1 %d %s\r\n", psParser->status_code, psRR->pcStatMes) ;
	iRV += socprintf(&psRR->sCtx, "Date: %#Z\r\n", &sTSZ) ;
	iRV += socprintf(&psRR->sCtx, "Content-Language: en-US\r\n") ;
	if (psRR->hvConnect) {
		iRV += socprintf(&psRR->sCtx, "Connection: %s\r\n", coValues[psRR->hvConnect]) ;
		if (psRR->hvConnect == coKeepAlive) {
			iRV += socprintf(&psRR->sCtx, "Keep-Alive: timeout=3\r\n") ;
		}
	}
	if (psRR->hvContentType) {
		iRV += socprintf(&psRR->sCtx, "Content-Type: %s\r\n", ctValues[psRR->hvContentType]) ;
	}

	va_list vArgs ;
	va_start(vArgs, format) ;
	/* The next line does not really print anything. By specifying the NULL as the string
	 * pointer it performs a test print and simply return the number of characters that
	 * would have been printed if a destination was specified.*/
	psRR->hvContentLength = xvsprintf(NULL, format, vArgs) ;
	/* Now do the actual formatted print of the content length with an additional 2 chars
	 * added for the extra CR + LF pair to form the blank line after the header values */
	iRV += socprintf(&psRR->sCtx, "Content-Length: %d\r\n\r\n", psRR->hvContentLength + 2) ;
	iRV += vsocprintf(&psRR->sCtx, format, vArgs) ;				// add the actual content
	va_end(vArgs) ;
	iRV += socprintf(&psRR->sCtx, "\r\n") ;						// add the final CR+LF after the body

	IF_CPRINT((myDEBUG == 1) && psRR->f_debug, "Content:\n%.*s", psRR->sBuf.Used, psRR->sBuf.pBuf) ;
	return iRV ;
}

int32_t	xHttpServerParseWriteString(char * pKey, char *pVal) {
	if (xStringParseEncoded(pVal, NULL) == erFAILURE) {
		return erFAILURE ;
	}
	IF_CPRINT(debugTRACK, "%s->%s\n", pKey, pVal) ;
	return halSTORAGE_WriteKeyValue(halSTORAGE_STORE, pKey, (x32_t) pVal, vfSXX) == ESP_OK ? erSUCCESS : erFAILURE ;
}

int32_t	xHttpServerParseString(char * pVal, char * pDst) {
	if (xStringParseEncoded(pVal, pDst) == erFAILURE) {
		return erFAILURE ;
	}
	IF_CPRINT(debugTRACK, "%s->%s\n", pVal, pDst) ;
	return erSUCCESS ;
}

int32_t	xHttpServerParseWriteIPaddress(char * pKey, char * pVal) {
	if (xStringParseEncoded(pVal, NULL) == erFAILURE) {
		return erFAILURE ;
	}
	IF_CPRINT(debugTRACK, "%s->%s", pKey, pVal) ;
	uint32_t	IPaddr ;
	if (pcStringParseIpAddr(pVal, &IPaddr) == pcFAILURE) {
		return erFAILURE ;
	}
	IF_CPRINT(debugTRACK, " : %-I\n", IPaddr) ;
	return halSTORAGE_WriteKeyValue(halSTORAGE_STORE, pKey, (x32_t) htonl(IPaddr), vfUXX) == ESP_OK ? erSUCCESS : erFAILURE ;
}

int32_t	xHttpServerParseIPaddress(char * pVal, uint32_t * pDst) {
	if (xStringParseEncoded(pVal, NULL) == erFAILURE) {
		return erFAILURE ;
	}
	IF_CPRINT(debugTRACK, "%s->%s", pVal) ;
	if (pcStringParseIpAddr(pVal, pDst) == pcFAILURE) {
		*pDst = 0 ;
		return erFAILURE ;
	}
	IF_CPRINT(debugTRACK, " : %-I\n", *pDst) ;
	return erSUCCESS ;
}

// ######################################## URL handlers ###########################################

int32_t	xHttpHandle_API(http_parser * psParser) {
	static const char format[] = "<html><body><h2>Function result</h2><pre>%.*s</pre></body></html>" ;
	http_reqres_t * psRR = psParser->data ;
	int32_t iRV ;
	vCommandInterpret(1, (int) *psRR->parts[1]) ;
	if (xUBufAvail(&sBufStdOut) > 0) {
		iRV = xHttpSendResponse(psParser, format, xUBufAvail(&sBufStdOut), pcUBufTellRead(&sBufStdOut)) ;
		vUBufReset(&sBufStdOut) ;
	} else {
		iRV = xHttpSendResponse(psParser, "<html><body><h2>Command completed</h2></body></html>") ;
	}
	return iRV ;
}

// ################################### Common HTTP API functions ###################################

void	vHttpServerCloseClient(sock_ctx_t * psCtx) {
	vRtosClearStatus(flagNET_HTTP_CLNT) ;
	HttpState = stateHTTP_WAITING ;
	xNetClose(psCtx) ;
	IF_CPRINT(debugTRACK, "closing\n") ;
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

	int32_t	iURL = -1, iRetVal ;
	if (psParser->http_errno) {
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_NOT_ACCEPTABLE) ;
		psRR->pcBody	= (char *) http_errno_description(HTTP_PARSER_ERRNO(psParser)) ;
		psRR->hvContentType = ctTextPlain ;
		SL_ERR("%s (%s)", psRR->pcBody, http_errno_name(HTTP_PARSER_ERRNO(psParser))) ;

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
			iURL = xHttpCommonFindMatch(UrlTable, NUM_OF_MEMBERS(UrlTable), psRR->url.path, strlen(psRR->url.path)) ;
			if (iURL == 0)	{
				iURL = urlNOTFOUND ;
			}
		}
	}

	switch(iURL) {
	case urlROOT:
		xHttpServerSetResponseStatus(psParser, HTTP_STATUS_OK) ;
		psRR->pcBody = (wifi_mode == WIFI_MODE_AP) ? (char *) HtmlAPdetails : (char *) HtmlSTAdetails ;
		break ;

	case urlSAVE_AP:
#if		(halNET_BUILD_DHCP == 1) || (halNET_BUILD_AUTO == 1)
		if ((strcmp(psRR->params[0].key, halSTORAGE_KEY_SSID) != 0)	||
			(strcmp(psRR->params[1].key, halSTORAGE_KEY_PSWD) != 0))
#elif	(halNET_BUILD_STATIC == 1)
		if ((strcmp(psRR->params[0].key, halSTORAGE_KEY_SSID) != 0) ||
			(strcmp(psRR->params[1].key, halSTORAGE_KEY_PSWD) != 0)	||
			(strcmp(psRR->params[2].key, halSTORAGE_KEY_NM)	!= 0)	||
			(strcmp(psRR->params[3].key, halSTORAGE_KEY_GW) != 0)	||
			(strcmp(psRR->params[4].key, halSTORAGE_KEY_IP) != 0)	||
			(strcmp(psRR->params[5].key, halSTORAGE_KEY_DNS1) != 0)	||
			(strcmp(psRR->params[6].key, halSTORAGE_KEY_DNS2) != 0))
#endif
		{	xHttpServerSetResponseStatus(psParser, HTTP_STATUS_BAD_REQUEST) ;
			psRR->pcBody	= (char *) HtmlErrorBadQuery ;
		} else {
			memset(&nvsWifi, 0 , sizeof(nvsWifi)) ;
			iRetVal = xHttpServerParseString(psRR->params[0].val, (char *) nvsWifi.ssid) ;		// SSID
			if (iRetVal == erSUCCESS) {
				iRetVal = xHttpServerParseString(psRR->params[1].val, (char *) nvsWifi.pswd) ;	// PSWD
#if		(halNET_BUILD_STATIC == 1)
				if (iRetVal == erSUCCESS) {					// Network Address
					iRetVal = xHttpServerParseIPaddress(psRR->params[2].val, &nvsWifi.ipNM) ;
					if (iRetVal == erSUCCESS) {				// Gateway IP
						iRetVal = xHttpServerParseIPaddress(psRR->params[3].val, &nvsWifi.ipGW) ;
						if (iRetVal == erSUCCESS) {			// Station IP
							iRetVal = xHttpServerParseIPaddress(psRR->params[4].val, &nvsWifi.ipSTA) ;
							if (iRetVal == erSUCCESS) {		// DNS IP #1
								iRetVal = xHttpServerParseIPaddress(psRR->params[5].val, &nvsWifi.ipDNS1) ;
								if (iRetVal == erSUCCESS) {	// DNS IP #2
									iRetVal = xHttpServerParseIPaddress(psRR->params[6].val, &nvsWifi.ipDNS2) ;
								}
							}
						}
					}
				}
#endif
				halSTORAGE_WriteBlob(halSTORAGE_STORE, halSTORAGE_KEY_WIFI, &nvsWifi, sizeof(nvsWifi)) ;
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
		iRetVal = psRR->hdlr_rsp(psParser) ;			// Add dynamic content to buffer via callback
	} else {
		iRetVal = xHttpSendResponse(psParser, psRR->pcBody) ;
		IF_CPRINT(debugTRACK, "Response sent iRV=%d\n", iRetVal) ;
	}
	if (sServHttpCtx.maxTx < iRetVal) {
		sServHttpCtx.maxTx = iRetVal ;
	}

	return iRetVal ;
}

/**
 *vTaskHttp()
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
void	vTaskHttp(void * pvParameters) {
	IF_TRACK(debugAPPL_THREADS, debugAPPL_MESS_UP) ;
	sRR.sBuf.pBuf	= pvPortMalloc(sRR.sBuf.Size = httpSERVER_BUFSIZE) ;
	HttpState 		= stateHTTP_INIT ;
	vRtosSetRunState(taskHTTP) ;

	while (xRtosVerifyState(taskHTTP)) {
		vRtosWaitStatus(flagNET_L3) ;				// ensure IP is up and running...
		switch(HttpState) {
		int32_t	iRetVal ;
		case stateHTTP_DEINIT:
			IF_CPRINT(debugTRACK, "de-init\n") ;
			vRtosClearStatus(flagNET_HTTP_SERV | flagNET_HTTP_CLNT) ;
			xNetClose(&sRR.sCtx) ;
			xNetClose(&sServHttpCtx) ;
			HttpState = stateHTTP_INIT ;
			/* no break */

		case stateHTTP_INIT:
			IF_CPRINT(debugTRACK, "init\n") ;
			memset(&sServHttpCtx, 0 , sizeof(sock_ctx_t)) ;
			sServHttpCtx.sa_in.sin_family		= AF_INET ;
			sServHttpCtx.type					= SOCK_STREAM ;
#if 0
//			sServHttpCtx.psSec					= ? ;
//			sServHttpCtx.d_data					= 1 ;
//			sServHttpCtx.d_read					= 1 ;
//			sServHttpCtx.d_write				= 1 ;
#endif
			sServHttpCtx.sa_in.sin_port			= htons(sServHttpCtx.psSec ? IP_PORT_HTTPS : IP_PORT_HTTP) ;
			iRetVal = xNetOpen(&sServHttpCtx) ;
			if (iRetVal < erSUCCESS) {
				HttpState = stateHTTP_DEINIT ;
				break ;
			}
			vRtosSetStatus(flagNET_HTTP_SERV) ;
			HttpState = stateHTTP_WAITING ;
			IF_CPRINT(debugTRACK, "waiting\n") ;
			/* no break */

		case stateHTTP_WAITING:
			iRetVal = xNetAccept(&sServHttpCtx, &sRR.sCtx, httpINTERVAL_MS) ;
			if (iRetVal < 0) {
				if (sServHttpCtx.error != EAGAIN) {
					HttpState = stateHTTP_DEINIT ;
				}
				break ;
			}

			iRetVal = xNetSetRecvTimeOut(&sRR.sCtx, httpINTERVAL_MS) ;
			if (iRetVal != erSUCCESS) {
				HttpState = stateHTTP_DEINIT ;
				break ;
			}
			vRtosSetStatus(flagNET_HTTP_CLNT) ;		// mark as having a client connection
			HttpState = stateHTTP_CONNECTED ;
			IF_CPRINT(debugTRACK, "connected\n") ;
			/* no break */

		case stateHTTP_CONNECTED:
			iRetVal = xNetRead(&sRR.sCtx, sRR.sBuf.pBuf, sRR.sBuf.Size) ;
			if (iRetVal > 0) {							// read something ?
				if (sServHttpCtx.maxRx < iRetVal) {		// yes, update the Rx packet stats
					sServHttpCtx.maxRx = iRetVal ;
				}
				http_parser 	sParser ;				// then process the packet
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

				sRR.sBuf.Used		= iRetVal ;
				iRetVal = xHttpCommonDoParsing(&sParser) ;
				if (iRetVal > 0) {						// build response if something was parsed....
					xStdOutLock(portMAX_DELAY) ;
					iRetVal = xHttpServerResponseHandler(&sParser) ;
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

		default:	SL_ERR(debugAPPL_PLACE) ;
		}
		vTaskDelay(pdMS_TO_TICKS(httpINTERVAL_MS)) ;
	}
	vPortFree(sRR.sBuf.pBuf) ;
	xNetClose(&sServHttpCtx) ;
	xNetClose(&sRR.sCtx) ;
	IF_TRACK(debugAPPL_THREADS, debugAPPL_MESS_DN) ;
	vTaskDelete(NULL) ;
}

void	vTaskHttpInit(void) { xRtosTaskCreate(vTaskHttp, "HTTP", httpSTACK_SIZE, NULL, httpPRIORITY, NULL, INT_MAX) ; }

void	vHttpReport(int32_t Handle) {
	if (xRtosCheckStatus(flagNET_HTTP_CLNT)) {
		xNetReport(Handle, &sRR.sCtx, __FUNCTION__, 0, 0, 0) ;
	}
	if (xRtosCheckStatus(flagNET_HTTP_SERV)) {
		xNetReport(Handle, &sServHttpCtx, __FUNCTION__, 0, 0, 0) ;
	}
	xdprintf(Handle, "HTTP Stats\tFSM=%d  maxTX=%u  maxRX=%u\n\n", HttpState, sServHttpCtx.maxTx, sServHttpCtx.maxRx) ;
}
