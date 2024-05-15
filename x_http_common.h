// x_http_common.h

#pragma once

#include "http_parser.h"
#include "socketsX.h"
#include "x_ubuf.h"
#include "yuarel.h"

#ifdef __cplusplus
extern "C" {
#endif

// ########################################### macros #############################################

#define	httpYUAREL_MAX_PARTS		8
#define	httpYUAREL_MAX_QUERY		8

#define	httpHDR_VALUES(ct, acc, con, d) ((ct << 24) | (acc << 16) | (con << 8) | d)

#define httpEOL	"\r\n"				// Linux = NL, macOS = CR,  Win = CRLF

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
};

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
};

enum {													// Connection Types
	coUNDEFINED					= 0,
	coKeepAlive,
	coUpgrade,
	coClose,
	coNUMBER,
};

enum {													// HTTP flags
	httpFLAG_FROM			= 1 << 0,
	httpFLAG_AGENT			= 1 << 1,
	httpFLAG_HOST			= 1 << 2,
	httpFLAG_STATUS			= 1 << 3,
	httpFLAG_0				= 1 << 4,
	httpFLAG_1				= 1 << 5,
	httpFLAG_2				= 1 << 6,
	httpFLAG_DEBUG			= 1 << 7,
};

// ######################################### structures ############################################

typedef struct http_rr_t {
	ubuf_t sUB;											// both
	netx_t sCtx;										// both
	/* Sequence of parameters in pVarArg MUST be in same sequence as used by
	 * a) the pcQuery format string; and
	 * b) the pcBody format string
	 */
	union {
		pcc_t pcQuery;									// client: 'format' GET/PUT/POST/DELETE/PATCH .....
		pcc_t pcStatMes;								// server: status message
	};
	union {												// body related
		void * pvBody;
 		const char * pcBody;							// datasize == 0 then content/format
		int (* cbBody) (struct http_rr_t *);			// datasize != 0 then content handler
	};
	va_list VaList;										// Client
	void * pvArg;										// Client
	http_parser_settings sfCB;							// Both
	struct yuarel url;									// Both
	struct yuarel_param	params[httpYUAREL_MAX_QUERY];	// Both
	char * parts[httpYUAREL_MAX_PARTS];					// Both
	u64_t hvContentLength;								// Both
	u32_t hvDate;										// Both
	u32_t hvLastModified;								// Both
	char * hvStatusMess;								// Both
	union {
		struct __attribute__((packed)) { 
			u8_t Spare, hvConnect, hvAccept, hvContentType;
		};
		u32_t hvValues;
	};
	u16_t hvStatus;										// Client (response to request)
	u8_t HdrField;										// Both
	s8_t NumParts;										// recognize -1 as error/none
	s8_t NumQuery;										// recognize -1 as error/none
	union {
		struct __attribute__((packed)) {
			u8_t f_parts:1;								// set to break URL up into parts
			u8_t f_query:1;								// set to break query up into parts
			u8_t f_bodyCB:1;							// set if pcBody contains a CB handler
			u8_t f_host:1;								// host info provided, or not ?
			u8_t f_ac_rng:1;							// accept ranges
		};
		u8_t f_allflags;
	};
} http_rr_t;

typedef int (* hdlr_req_t) (struct http_rr_t *);

// ################################### Global variables ############################################
// ###################################### public functions #########################################

int	xHttpCommonFindMatch(const char * const pcTable[], u32_t xSize, const char * pcMatch, size_t xLen);
int	xHttpCommonMessageBeginHandler(http_parser * psParser);
int xHttpCommonUrlHandler(http_parser * psParser, const char * pBuf, size_t xLen);
int xHttpCommonStatusHandler(http_parser * psParser, const char * pBuf, size_t xLen);
int xHttpCommonHeaderFieldHandler(http_parser * psParser, const char * pBuf, size_t xLen);
int xHttpCommonHeaderValueHandler(http_parser * psParser, const char * pBuf, size_t xLen);
int xHttpCommonHeadersCompleteHandler(http_parser * psParser);
int xHttpCommonChunkHeaderHandler(http_parser * psParser);
int xHttpCommonChunkCompleteHandler(http_parser * psParser);
int xHttpCommonMessageBodyHandler(http_parser * psParser, const char * pBuf, size_t xLen);
int xHttpCommonMessageCompleteHandler(http_parser * psParser);
size_t	xHttpCommonDoParsing(http_parser * psParser);

#ifdef __cplusplus
}
#endif
