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
 * http_server.h
 */

#pragma once

#include 	"http_common.h"
#include	<stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ###################################### BUILD : CONFIG definitions ###############################

#define	httpPRIORITY					4
#define	httpSTACK_SIZE					(configMINIMAL_STACK_SIZE + 1280 + (myDEBUG * 256))
#define	httpINTERVAL_MS					(500)

#define httpSERVER_BUFSIZE				(1 * KILO)

// ########################################### macros #############################################


// ######################################### enumerations ##########################################

enum { stateHTTP_DEINIT = 1, stateHTTP_INIT, stateHTTP_WAITING, stateHTTP_CONNECTED } ;

// ######################################### structures ############################################


// ################################### Global variables ############################################

extern	sock_ctx_t	sServHttpCtx ;
extern	http_reqres_t	sRR ;

// ###################################### public functions #########################################

int32_t xHttpServerSetResponseStatus(http_parser * psParser, int32_t Status) ;

void	vTaskHttp(void * pvParameters) ;
void	vTaskHttpInit(void) ;
void	vHttpReport(void) ;

#ifdef __cplusplus
}
#endif
