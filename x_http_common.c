/*
 * http_common.c
 * Copyright (c) 2014-22 Andre M. Maree / KSS Technologies (Pty) Ltd.
 */

#include "hal_config.h"
#include "x_http_common.h"
#include "parserX.h"
#include "x_buffers.h"
#include "printfx.h"
#include "syslog.h"
#include "x_string_to_values.h"
#include "x_string_general.h"
#include "x_errors_events.h"

// ############################### BUILD: debug configuration options ##############################

#define	debugFLAG					0xF000

#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ########################################### macros ##############################################


// ###################################### local ie static variables ################################


// ###################################### global variables #########################################

const char * const hfValues[hfNUMBER]	= {
	[hfUNDEFINED]						= "",
	[hfAccept]							= "accept",
	[hfAcceptCharset]					= "accept-charset",
	[hfAcceptDatetime]					= "accept-datetime",
	[hfAcceptEncoding]					= "accept-encoding",
	[hfAcceptLanguage]					= "accept-language",
	[hfAcceptPatch]						= "accept-patch",
	[hfAcceptRanges]					= "accept-ranges",
	[hfAuthorisation]					= "authorization",
	[hfCacheControl]					= "cache-control",
	[hfConnection]						= "connection",
	[hfContentLength]					= "content-length",
	[hfContentType]						= "content-type",
	[hfDate]							= "date",
	[hfHost]							= "host",
	[hfLastModified]					= "last-modified",
	[hfPragma]							= "pragma",
	[hfTransferEncoding]				= "transfer-encoding",
} ;

const char * const ctValues[ctNUMBER]	= {
	[ctUNDEFINED]						= "",
	[ctApplicationOctetStream]			= "application/octet-stream",
	[ctApplicationJson]					= "application/json",
	[ctApplicationXml]					= "application/xml",
	[ctApplicationXwwwFormUrlencoded]	= "application/x-www-form-urlencoded",
	[ctTextPlain]						= "text/plain",
	[ctTextHtml]						= "text/html",
} ;

const char * const coValues[coNUMBER]	= {
	[coUNDEFINED]						= "",
	[coKeepAlive]						= "keep-alive",
	[coUpgrade]							= "upgrade",
	[coClose]							= "close",
} ;

// ################################## local/static support functions ###############################

/* currently this function is used to find matches for both simple header fields,
 * always single string values without embeddded '/' or similar. The function is
 * also used by the HTTP server to do a simple match for URL's, brining in the limit
 * of only being able to match ie use single part URL's meaning "/api/something"
 * is not supported since it will not be matched */
int	xHttpCommonFindMatch(const char * const pcTable[], uint32_t xSize, const char * pcMatch, size_t xLen) {
	int	Idx = 0 ;
	while (Idx < xSize) {
		size_t ySize = strlen(*pcTable) ;				// get length of string in table to compare against
		if (ySize == xLen) {
			if (strncasecmp(*pcTable, pcMatch, ySize) == 0)
				return Idx;
		}
		pcTable++ ;
		Idx++ ;
	}
	return 0 ;											// should be ??UNDEFINED = 0
}

// ################################### Common HTTP API functions ###################################

int xHttpCommonMessageBeginHandler(http_parser * psP) {
	IF_P(debugTRACK && ((http_rr_t *) psP->data)->f_debug, "MESSAGE BEGIN\r\n") ;
	return erSUCCESS ;
}

int xHttpCommonUrlHandler(http_parser * psP, const char* pBuf, size_t xLen) {
//	P("BEFORE: pBuf=%p  xLen=%d  cChr='%c'  url=[%.*s]\r\n", pBuf, xLen, *(pBuf+xLen), xLen, pBuf) ;
	char * pTerm = (char *) pBuf + xLen ;				// overcome 'const'
	*pTerm = 0 ;										// terminate the string
//	P("AFTER : pBuf=%p  xLen=%d  cChr='%c'  url=[%s]\r\n", pBuf, xLen, *(pBuf+xLen), pBuf) ;

	http_rr_t * psRes = psP->data ;
	int Idx = yuarel_parse(&psRes->url, (char *)pBuf) ;		// do the parse
	if (Idx == erFAILURE) return erFAILURE ;

	/* This is to handle the special case where the whole URL is just "/"
	 * The yuarel parser then (incorrectly) returns " HTTP" and "1.1" as
	 * the first and second parts of the path */
	if ((*psRes->url.path != CHR_NUL) && (psRes->f_parts == 1)) {
		psRes->NumParts	= yuarel_split_path(psRes->url.path, (char**)psRes->parts, httpYUAREL_MAX_PARTS) ;
		psRes->NumParts = (psRes->NumParts == erFAILURE) ? 0 : psRes->NumParts ;
	} else {
		psRes->NumParts	= 0 ;
	}

	if (psRes->f_query == 1) {
		psRes->NumQuery	= yuarel_parse_query(psRes->url.query, CHR_AMPERSAND, psRes->params, httpYUAREL_MAX_QUERY) ;
		psRes->NumQuery = (psRes->NumQuery == erFAILURE) ? 0 : psRes->NumQuery ;
	} else {
		psRes->NumQuery = 0 ;
	}

	if (debugTRACK && psRes->f_debug) {
		printfx("Struct: scheme:%s  host:%s  port:%d  path:%s  query:%s  fragment:%s\r\n",
				psRes->url.scheme, psRes->url.host, psRes->url.port,
				*psRes->url.path == 0 ? "/" : psRes->url.path,
				psRes->url.query, psRes->url.fragment) ;
		for (Idx = 0; Idx < psRes->NumParts; ++Idx)
			printfx("Path part[%d]: '%s'\r\n", Idx, psRes->parts[Idx]) ;
		for (Idx = 0; Idx < psRes->NumQuery; ++Idx)
			printfx("Parameter[%d]: name='%s' value='%s'\r\n", Idx, psRes->params[Idx].key, psRes->params[Idx].val) ;
	}
	return erSUCCESS ;
}

int xHttpCommonStatusHandler(http_parser * psP, const char* pBuf, size_t xLen) {
	http_rr_t * psReq = psP->data ;
	IF_P(debugTRACK && psReq->f_debug, "Status: %d = '%.*s'\r\n", psP->status_code, xLen, pBuf) ;
	psReq->hvStatus		= psP->status_code ;
	*((char*) pBuf+xLen)= 0 ;
	psReq->hvStatusMess = (char *) pBuf ;
	return erSUCCESS ;
}

int xHttpCommonHeaderFieldHandler(http_parser * psP, const char* pBuf, size_t xLen) {
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psP) && halCONFIG_inSRAM(pBuf) && (xLen > 0)) ;
	http_rr_t * psReq = psP->data ;
	psReq->HdrField	= xHttpCommonFindMatch(hfValues, NO_MEM(hfValues), pBuf, xLen) ;
	IF_P(debugTRACK && psReq->f_debug, "'%.*s' = ", (int)xLen, pBuf);
	return erSUCCESS ;
}

int xHttpCommonHeaderValueHandler(http_parser * psP, const char* pBuf, size_t xLen) {
	http_rr_t * psReq = psP->data ;
	IF_P(debugTRACK && psReq->f_debug, "'%.*s'\r\n", (int)xLen, pBuf);
	struct tm sTM ;
	switch (psReq->HdrField) {
	case hfAcceptRanges:
		if (strncasecmp("bytes", pBuf, xLen) == 0) psReq->f_ac_rng = 1 ;
		break ;

	case hfConnection:
		psReq->hvConnect = xHttpCommonFindMatch(coValues, NO_MEM(coValues), pBuf, xLen) ;
		break ;

	case hfContentLength:
		psReq->hvContentLength = psP->content_length ;
		break ;

	case hfContentType:
		psReq->hvContentType = xHttpCommonFindMatch(ctValues, NO_MEM(ctValues), pBuf, xLen) ;
		break ;

	case hfDate:
		strptime(pBuf, "%a, %d %b %Y %T", &sTM) ;
		psReq->hvDate = mktime(&sTM) ;
		break ;

	case hfHost:
		psReq->f_host = 1 ;
		break ;

	case hfLastModified:
		strptime(pBuf, "%a, %d %b %Y %T", &sTM) ;
		psReq->hvLastModified = mktime(&sTM) ;
		break ;

	default:
		break ;
	}
	return erSUCCESS ;
}

int xHttpCommonHeadersCompleteHandler(http_parser * psP) {
	http_rr_t * psReq = psP->data ;
	IF_PX(debugTRACK && psReq->f_debug, "HEADERS COMPLETE: ar=%d  co=%d  ct=%d  host=%d  len=%llu  date=%R  last=%R\r\n",
			psReq->f_ac_rng, psReq->hvConnect, psReq->hvContentType, psReq->f_host, psP->content_length,
			xTimeMakeTimestamp(psReq->hvDate, 0), xTimeMakeTimestamp(psReq->hvLastModified, 0)) ;
	// if we return 1 here the parser will believe there is no body in the message
	return erSUCCESS ;
}

int xHttpCommonChunkHeaderHandler(http_parser * psP) {
	IF_P(debugTRACK && ((http_rr_t *) psP->data)->f_debug, "CHUNK HEADER\r\n") ;
	return erSUCCESS ;
}

int	xHttpCommonChunkCompleteHandler(http_parser * psP) {
	IF_P(debugTRACK && ((http_rr_t *) psP->data)->f_debug, "CHUNK COMPLETE\r\n") ;
	return erSUCCESS ;
}

int xHttpCommonMessageBodyHandler(http_parser * psP, const char * pcBuf, size_t xLen) {
	http_rr_t * psReq = psP->data ;
	switch (psReq->hvContentType) {
	case ctTextPlain:
	case ctTextHtml:
	case ctApplicationXml:
		IF_P(debugTRACK && psReq->f_debug, "BODY (plain/html/xml)\r\n%.*s", xLen, pcBuf) ;
		break ;
	case ctApplicationJson:
	{	// test parse (count tokens) then allocate memory & parse
		jsmntok_t *	psTokenList ;
		jsmn_parser	sParser ;
		int32_t iRV = xJsonParse(pcBuf, xLen, &sParser, &psTokenList) ;
		if (iRV > erSUCCESS) {							// print parsed tokens
			iRV = xJsonPrintTokens(pcBuf, psTokenList, iRV, 0) ;
		} else {
			IF_PX(debugTRACK && psReq->f_debug, "BODY (json)\r\n%!'+hhY", xLen, pcBuf) ;	// not parsed, just dump...
		}
		if (psTokenList) vRtosFree(psTokenList) ;
		break ;
	}
	default:
		IF_PX(debugTRACK && psReq->f_debug, "BODY (other)\r\n%!'+hhY", xLen, pcBuf) ;
	}
    return erSUCCESS ;
}

int xHttpCommonMessageCompleteHandler(http_parser * psP) {
	IF_P(debugTRACK && ((http_rr_t *) psP->data)->f_debug, "MESSAGE COMPLETE\r\n") ;
	return erSUCCESS ;
}

size_t	xHttpCommonDoParsing(http_parser * psP) {
	http_rr_t * psRR = psP->data ;
	psRR->sfCB.on_url			= xHttpCommonUrlHandler ;	// set some default handlers...
	psRR->sfCB.on_status		= xHttpCommonStatusHandler ;
	psRR->sfCB.on_header_field	= xHttpCommonHeaderFieldHandler ;
	psRR->sfCB.on_header_value	= xHttpCommonHeaderValueHandler ;
	if (debugTRACK && psRR->sfCB.on_body == NULL) {
		psRR->sfCB.on_body		= xHttpCommonMessageBodyHandler ;
	}
	if (debugTRACK && psRR->f_debug) {
		psRR->sfCB.on_message_begin		= xHttpCommonMessageBeginHandler ;
		psRR->sfCB.on_chunk_header		= xHttpCommonChunkHeaderHandler ;
		psRR->sfCB.on_chunk_complete	= xHttpCommonChunkCompleteHandler ;
		psRR->sfCB.on_headers_complete	= xHttpCommonHeadersCompleteHandler ;
		psRR->sfCB.on_message_complete	= xHttpCommonMessageCompleteHandler ;
	}

	int iRV = http_parser_execute(psP, &psRR->sfCB, (char *)psRR->sUB.pBuf, psRR->sUB.Used);
	if (psRR->f_debug) {
		if (iRV <= 0) {
			SL_NOT("parse %s (%s) url=%s/%s/%s", http_errno_name(HTTP_PARSER_ERRNO(psP)),
				http_errno_description(HTTP_PARSER_ERRNO(psP)),
				psRR->url.host, psRR->url.path, psRR->url.query);
			iRV = erFAILURE;
		}
		if (INRANGE(HTTP_STATUS_BAD_REQUEST, psP->status_code, HTTP_STATUS_NETWORK_AUTHENTICATION_REQUIRED)) {
			SL_NOT("http error=%d (%s)", psP->status_code, psRR->hvStatusMess);
			iRV = erFAILURE;
		}
	}
	return iRV;
}
