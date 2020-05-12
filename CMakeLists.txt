idf_component_register(	SRCS x_http_client.c x_http_common.c x_http_server.c 
						INCLUDE_DIRS .
						REQUIRES irmacos x_sockets common http-parser libyuarel buffers
						PRIV_REQUIRES hal_esp32 rtos-support statistics sitewhere actuators x_json common syslog printf systiming hal_esp32 commands
						)
