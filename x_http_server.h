// x_http_server.h

#pragma once

#include "x_http_common.h"
#include "FreeRTOS_Support.h"
#include "http_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

// ###################################### BUILD : CONFIG definitions ###############################

// ########################################### macros #############################################

// ######################################### enumerations ##########################################

enum { stateHTTP_DEINIT = 1, stateHTTP_INIT, stateHTTP_WAITING, stateHTTP_CONNECTED };

// ######################################### structures ############################################

// ################################### Global variables ############################################

// ###################################### public functions #########################################

void vHttpStartStop(void);
struct report_t;
void vHttpReport(struct report_t * psR);

#ifdef __cplusplus
}
#endif
