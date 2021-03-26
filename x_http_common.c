/*
 * Copyright 2014-21 Andre M. Maree / KSS Technologies (Pty) Ltd.
 */

/*
 * http_common.c
 */

#include	"hal_config.h"
#include 	"x_http_common.h"
#include	"FreeRTOS_Support.h"

#include	"parserX.h"
#include	"x_buffers.h"
#include	"x_string_to_values.h"
#include	"x_string_general.h"
#include	"x_errors_events.h"
#include	"printfx.h"

#include	<stdlib.h>
#include	<stdint.h>
#include	<string.h>

// ############################### BUILD: debug configuration options ##############################

#define	debugFLAG					0xE000

#define	debugPARSE					(debugFLAG & 0x0001)
#define	debugURL					(debugFLAG & 0x0002)
#define	debugBUILD					(debugFLAG & 0x0004)

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

/* currently this function is used to find matched for both simple header fields,
 * always single string values without embeddded '/' or similar. The function is
 * also used by the HTTP server to do a simple match for URL's, brining in the limit
 * of only being able to match ie use single part URL's meaning "/api/something"
 * is not supported since it will not be matched */
int32_t	xHttpCommonFindMatch(const char * const pcTable[], uint32_t xSize, const char * pcMatch, size_t xLen) {
	int32_t	Idx = 0 ;
	while (Idx < xSize) {
		int32_t ySize = xstrlen(*pcTable) ;				// get length of string in table to compare against
		if (ySize == xLen) {
			IF_PRINT(debugPARSE, "#%d '%s' vs '%.*s'\n", Idx, *pcTable, ySize, pcMatch) ;
			if (strncasecmp(*pcTable, pcMatch, ySize) == 0) {
				IF_PRINT(debugPARSE, "#%d '%s' vs '%.*s'\n", Idx, *pcTable, ySize, pcMatch) ;
				return (Idx) ;
			}
		}
		pcTable++ ;
		Idx++ ;
	}
	return 0 ;											// should be ??UNDEFINED = 0
}

// ################################### Common HTTP API functions ###################################

int 	xHttpCommonMessageBeginHandler(http_parser * psParser) {
	IF_PRINT(debugTRACK && ((http_rr_t *) psParser->data)->f_debug, "MESSAGE BEGIN\n") ;
	return erSUCCESS ;
}

int 	xHttpCommonUrlHandler(http_parser * psParser, const char* pBuf, size_t xLen) {
	IF_PRINT(debugURL, "BEFORE: pBuf=%p  xLen=%d  cChr='%c'  url=[%.*s]\n", pBuf, xLen, *(pBuf+xLen), xLen, pBuf) ;
	char * pTerm = (char *) pBuf + xLen ;				// overcome 'const'
	*pTerm = 0 ;										// terminate the string
	IF_PRINT(debugURL, "AFTER : pBuf=%p  xLen=%d  cChr='%c'  url=[%s]\n", pBuf, xLen, *(pBuf+xLen), pBuf) ;

	http_rr_t * psRes = psParser->data ;
	int32_t Idx = yuarel_parse(&psRes->url, (char *)pBuf) ;		// do the parse
	if (Idx == erFAILURE) {
		return erFAILURE ;
	}

	/* This is to handle the special case where the whole URL is just "/"
	 * The yuarel parser then (incorrectly) returns " HTTP" and "1.1" as
	 * the first and second parts of the path */
	if ((*psRes->url.path != CHR_NUL) && (psRes->f_parts == 1)) {
		psRes->NumParts	= yuarel_split_path(psRes->url.path, psRes->parts, httpYUAREL_MAX_PARTS) ;
		psRes->NumParts = (psRes->NumParts == erFAILURE) ? 0 : psRes->NumParts ;
	} else {
		psRes->NumParts	= 0 ;
	}

	if (psRes->f_query == 1) {
		psRes->NumQuery	= yuarel_parse_query(psRes->url.query, '&', psRes->params, httpYUAREL_MAX_QUERY) ;
		psRes->NumQuery = (psRes->NumQuery == erFAILURE) ? 0 : psRes->NumQuery ;
	} else {
		psRes->NumQuery = 0 ;
	}

	if (debugTRACK && psRes->f_debug) {
		printfx("Struct: scheme:%s  host:%s  port:%d  path:%s  query:%s  fragment:%s\n",
				psRes->url.scheme, psRes->url.host, psRes->url.port,
				*psRes->url.path == CHR_NUL ? "/" : psRes->url.path,
				psRes->url.query, psRes->url.fragment) ;
		for (Idx = 0; Idx < psRes->NumParts; ++Idx)
			printfx("Path part[%d]: '%s'\n", Idx, psRes->parts[Idx]) ;
		for (Idx = 0; Idx < psRes->NumQuery; ++Idx)
			printfx("Parameter[%d]: name='%s' value='%s'\n", Idx, psRes->params[Idx].key, psRes->params[Idx].val) ;
	}
	return erSUCCESS ;
}

int 	xHttpCommonStatusHandler(http_parser * psParser, const char* pBuf, size_t xLen) {
	http_rr_t * psReq = psParser->data ;
	IF_PRINT(debugTRACK && psReq->f_debug, "Status: %d = '%.*s'\n", psParser->status_code, xLen, pBuf) ;
	psReq->hvStatus		= psParser->status_code ;
	*((char*) pBuf+xLen)= CHR_NUL ;
	psReq->hvStatusMess = (char *) pBuf ;
	return erSUCCESS ;
}

int 	xHttpCommonHeaderFieldHandler(http_parser * psParser, const char* pBuf, size_t xLen) {
	IF_myASSERT(debugPARAM, halCONFIG_inSRAM(psParser) && halCONFIG_inSRAM(pBuf) && (xLen > 0)) ;
	http_rr_t * psReq = psParser->data ;
	psReq->HdrField	= xHttpCommonFindMatch(hfValues, NUM_OF_MEMBERS(hfValues), pBuf, xLen) ;
	IF_PRINT(debugTRACK && psReq->f_debug, "'%.*s' = ", (int)xLen, pBuf);
	return erSUCCESS ;
}

int 	xHttpCommonHeaderValueHandler(http_parser * psParser, const char* pBuf, size_t xLen) {
	http_rr_t * psReq = psParser->data ;
	IF_PRINT(debugTRACK && psReq->f_debug, "'%.*s'\n", (int)xLen, pBuf);
	struct tm sTM ;
	switch (psReq->HdrField) {
	case hfAcceptRanges:
		if (strncasecmp("bytes", pBuf, xLen) == 0) {
			psReq->f_ac_rng = 1 ;
		}
		break ;

	case hfConnection:
		psReq->hvConnect = xHttpCommonFindMatch(coValues, NUM_OF_MEMBERS(coValues), pBuf, xLen) ;
		break ;

	case hfContentLength:
		psReq->hvContentLength = psParser->content_length ;
		break ;

	case hfContentType:
		psReq->hvContentType = xHttpCommonFindMatch(ctValues, NUM_OF_MEMBERS(ctValues), pBuf, xLen) ;
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

int 	xHttpCommonHeadersCompleteHandler(http_parser * psParser) {
	http_rr_t * psReq = psParser->data ;
	IF_PRINT(debugTRACK && psReq->f_debug, "HEADERS COMPLETE: ar=%d  co=%d  ct=%d  host=%d  len=%llu  date=%R  last=%R\n",
			psReq->f_ac_rng, psReq->hvConnect, psReq->hvContentType, psReq->f_host, psParser->content_length,
			xTimeMakeTimestamp(psReq->hvDate, 0), xTimeMakeTimestamp(psReq->hvLastModified, 0)) ;
	// if we return 1 here the parser will believe there is no body in the message
	return erSUCCESS ;
}

int 	xHttpCommonChunkHeaderHandler(http_parser * psParser) {
	IF_PRINT(debugTRACK && ((http_rr_t *) psParser->data)->f_debug, "CHUNK HEADER\n") ;
	return erSUCCESS ;
}

int		xHttpCommonChunkCompleteHandler(http_parser * psParser) {
	IF_PRINT(debugTRACK && ((http_rr_t *) psParser->data)->f_debug, "CHUNK COMPLETE\n") ;
	return erSUCCESS ;
}

/**
 * xHttpCommonMessageBodyHandler() default handler for body content
 * @brief	If content is JSON format, will parse and display, else just debug dump
 * @param	psParser
 * @param	pBuf
 * @param	xLen
 * @return
 */
int 	xHttpCommonMessageBodyHandler(http_parser * psParser, const char * pcBuf, size_t xLen) {
	http_rr_t * psReq = psParser->data ;
	switch (psReq->hvContentType) {
	case ctTextPlain:
	case ctTextHtml:
	case ctApplicationXml:
		IF_PRINT(debugTRACK, "BODY (plain/html/xml)\n%.*s", xLen, pcBuf) ;
		break ;
	case ctApplicationJson:
	{	// test parse (count tokens) then allocate memory & parse
		jsmntok_t *	psTokenList ;
		jsmn_parser	sParser ;
		int32_t iRV = xJsonParse(pcBuf, xLen, &sParser, &psTokenList) ;
		if (iRV > erSUCCESS) {							// print parsed tokens
			iRV = xJsonPrintTokens(pcBuf, psTokenList, iRV, 0) ;
		} else {
			IF_PRINT(debugTRACK, "BODY (json)\n%!'+b", xLen, pcBuf) ;	// not parsed, just dump...
		}
		if (psTokenList) {								// if allocated,
			vPortFree(psTokenList) ;					// free the memory allocated in xJsonParse()
		}
		break ;
	}
	default:
		IF_PRINT(debugTRACK, "BODY (other)\n%!'+b", xLen, pcBuf) ;
	}
    return erSUCCESS ;
}

int 	xHttpCommonMessageCompleteHandler(http_parser * psParser) {
	IF_PRINT(debugTRACK && ((http_rr_t *) psParser->data)->f_debug, "MESSAGE COMPLETE\n") ;
	return erSUCCESS ;
}

/**
 * xHttpCommonDoParsing()
 * @param	psParser
 * @return	erFAILURE or result of http_parser_execute() being 0, 1 or some number ?
 */
size_t	xHttpCommonDoParsing(http_parser * psParser) {
	http_rr_t * psRR = psParser->data ;
	psRR->sfCB.on_url			= xHttpCommonUrlHandler ;	// set some default handlers...
	psRR->sfCB.on_status		= xHttpCommonStatusHandler ;
	psRR->sfCB.on_header_field	= xHttpCommonHeaderFieldHandler ;
	psRR->sfCB.on_header_value	= xHttpCommonHeaderValueHandler ;
	if (debugTRACK && psRR->sfCB.on_body == NULL)
		psRR->sfCB.on_body		= xHttpCommonMessageBodyHandler ;

	if (debugTRACK && psRR->f_debug) {
		psRR->sfCB.on_message_begin		= xHttpCommonMessageBeginHandler ;
		psRR->sfCB.on_chunk_header		= xHttpCommonChunkHeaderHandler ;
		psRR->sfCB.on_chunk_complete	= xHttpCommonChunkCompleteHandler ;
		psRR->sfCB.on_headers_complete	= xHttpCommonHeadersCompleteHandler ;
		psRR->sfCB.on_message_complete	= xHttpCommonMessageCompleteHandler ;
	}

	int32_t iRV = http_parser_execute(psParser, &psRR->sfCB, psRR->sUB.pBuf, psRR->sUB.Used) ;
	if (psRR->f_debug) {
		if (iRV <= 0) {
			IF_PRINT(debugRESULT, "parse %s (%s) url=%s/%s/%s\n",
					http_errno_name(HTTP_PARSER_ERRNO(psParser)),
					http_errno_description(HTTP_PARSER_ERRNO(psParser)),
					psRR->url.host, psRR->url.path, psRR->url.query) ;
			iRV = erFAILURE ;
		}
		if (INRANGE(HTTP_STATUS_BAD_REQUEST, psParser->status_code, HTTP_STATUS_NETWORK_AUTHENTICATION_REQUIRED, int16_t)) {
			IF_PRINT(debugRESULT, "http error=%d (%s)\n", psParser->status_code, psRR->hvStatusMess) ;
			iRV = erFAILURE ;
		}
	}
	return iRV ;
}
