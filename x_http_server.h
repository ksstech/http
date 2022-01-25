/*
 * Copyright 2014-21 Andre M. Maree / KSS Technologies (Pty) Ltd.
 * x_http_server.h
 */

#pragma once

#include	"hal_config.h"
#include	"FreeRTOS_Support.h"

#include 	"http_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

// ###################################### BUILD : CONFIG definitions ###############################

#define	httpPRIORITY				4
#define	httpSTACK_SIZE				(configMINIMAL_STACK_SIZE + 5120 + (flagSTACK * 256))

// ########################################### macros #############################################


// ######################################### enumerations ##########################################

enum { stateHTTP_DEINIT = 1, stateHTTP_INIT, stateHTTP_WAITING, stateHTTP_CONNECTED } ;

// ######################################### structures ############################################


// ################################### Global variables ############################################


// ###################################### public functions #########################################

int xHttpServerSetResponseStatus(http_parser * psParser, int Status) ;
void vTaskHttp(void * pvParameters) ;
void vTaskHttpStatus(void) ;
void vHttpReport(void) ;

#ifdef __cplusplus
}
#endif
