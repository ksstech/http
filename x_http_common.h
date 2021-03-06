/*
 * Copyright 2014-21 Andre. M Maree / KSS Technologies (Pty) Ltd.
 */

#pragma once

#include 	"http_parser.h"
#include	"yuarel.h"

#include	"x_ubuf.h"
#include	"socketsX.h"

#ifdef __cplusplus
extern "C" {
#endif

// ########################################### macros #############################################

#define	httpYUAREL_MAX_PARTS		8
#define	httpYUAREL_MAX_QUERY		8

// ######################################### enumerations ##########################################

enum {													// Header Fields
	hfUNDEFINED					= 0,
	hfAccept,
	hfAcceptCharset,
	hfAcceptDatetime,
	hfAcceptEncoding,
	hfAcceptLanguage,
	hfAcceptPatch,
	hfAcceptRanges,
	hfAuthorisation,
	hfCacheControl,
	hfConnection,
	hfContentLength,
	hfContentType,
	hfDate,
	hfHost,
	hfLastModified,
	hfPragma,
	hfTransferEncoding,
	hfNUMBER,
} ;

enum {													// Content Types
	ctUNDEFINED					= 0,
	ctApplicationOctetStream,
	ctApplicationJson,
	ctApplicationXml,
	ctTextPlain,
	ctTextHtml,
	ctApplicationXwwwFormUrlencoded,
#if 0													// below here not handled yet...
	ctApplicationPostScript,
	ctApplicationJavascript,
	ctApplicationZip,
	ctApplicationPdf,
	ctAudioMpeg,
	ctAudiovorbis,
	ctMultipartFormData,
	ctTextCss,
	ctImagePng,
	ctImageJpeg,
	ctImageGif,
#endif
	ctNUMBER,
} ;

enum {													// Connection Types
	coUNDEFINED					= 0,
	coKeepAlive,
	coUpgrade,
	coClose,
	coNUMBER,
} ;

enum {													// HTTP flags
	httpFLAG_FROM			= 1 << 0,
	httpFLAG_AGENT			= 1 << 1,
	httpFLAG_HOST			= 1 << 2,
	httpFLAG_STATUS			= 1 << 3,
	httpFLAG_0				= 1 << 4,
	httpFLAG_1				= 1 << 5,
	httpFLAG_2				= 1 << 6,
	httpFLAG_DEBUG			= 1 << 7,
} ;

// ######################################### structures ############################################

typedef struct http_rr_t {
	ubuf_t					sUB ;						// both
	netx_t					sCtx ;						// both
/* Sequence of parameters in pVarArg MUST be in same sequence as used by
 * a) the pcQuery format string; and
 * b) the pcBody format string
 */
	union {
		pci8_t	pcQuery ;								// client: 'format' GET/PUT/POST/DELETE/PATCH .....
		pci8_t	pcStatMes ;								// server: status message
	} ;
	union {
		const void * pVoid ;
		pci8_t	pcBody ;								// both (client 'format' string)
		int32_t (* hdlr_req) (struct http_rr_t *) ;		// client
		int32_t	(* hdlr_rsp) (http_parser *) ;			// server
	} ;
	va_list					VaList ;					// Client
	void *					pvArg ;						// Client
	http_parser_settings 	sfCB ;						// Both
	struct	yuarel			url ;						// Both
	struct	yuarel_param	params[httpYUAREL_MAX_QUERY] ;	// Both
	char *					parts[httpYUAREL_MAX_PARTS] ;	// Both
	uint64_t				hvContentLength ;			// Both
	uint32_t				hvDate ;					// Both
	uint32_t				hvLastModified ;			// Both
	char * 					hvStatusMess ;				// Both
	uint16_t				hvStatus ;					// Client (response to request)
	union {
		struct {
			uint8_t			Spare, hvConnect, hvAccept, hvContentType ;
		} ;
		uint32_t			hvValues ;
	};
	uint8_t					HdrField ;					// Both
	int8_t					NumParts ;					// recognize -1 as error/none
	int8_t					NumQuery ;					// recognize -1 as error/none
	union {
		struct {
			uint8_t	f_debug : 1 ;
			uint8_t	f_parts : 1 ;		// set to break URL up into parts
			uint8_t	f_query	: 1 ;		// set to break query up into parts
			uint8_t	f_bodyCB: 1 ;		// set if pcBody contains a CB handler
			uint8_t	f_host	: 1 ;		// host info provided, or not ?
			uint8_t	f_ac_rng: 1 ;		// accept ranges
		} ;
		uint8_t		f_allflags ;
	} ;
} http_rr_t ;

#define	httpHDR_VALUES(a,b,c,d) ((a<<24)|(b<<16)|(c<<8)|d)

// ################################### Global variables ############################################

extern	const char * const ctValues[] ;
extern	const char * const coValues[] ;
extern	const char * const hfValues[] ;

// ###################################### public functions #########################################

int32_t	xHttpCommonFindMatch(const char * const pcTable[], uint32_t xSize, const char * pcMatch, size_t xLen) ;

int		xHttpCommonMessageBeginHandler(http_parser * psParser) ;
int 	xHttpCommonUrlHandler(http_parser * psParser, const char * pBuf, size_t xLen) ;
int 	xHttpCommonStatusHandler(http_parser * psParser, const char * pBuf, size_t xLen) ;
int 	xHttpCommonHeaderFieldHandler(http_parser * psParser, const char * pBuf, size_t xLen) ;
int 	xHttpCommonHeaderValueHandler(http_parser * psParser, const char * pBuf, size_t xLen) ;
int 	xHttpCommonHeadersCompleteHandler(http_parser * psParser) ;
int 	xHttpCommonChunkHeaderHandler(http_parser * psParser) ;
int 	xHttpCommonChunkCompleteHandler(http_parser * psParser) ;
int 	xHttpCommonMessageBodyHandler(http_parser * psParser, const char * pBuf, size_t xLen) ;
int 	xHttpCommonMessageCompleteHandler(http_parser * psParser) ;

size_t	xHttpCommonDoParsing(http_parser * psParser) ;

#ifdef __cplusplus
}
#endif
