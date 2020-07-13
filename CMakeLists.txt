set(srcs "x_http_client.c" "x_http_common.c" "x_http_server.c" )
set(include_dirs "." )
#set(priv_include_dirs "." )
set(requires irmacos x_sockets common http-parser libyuarel buffers )
set(priv_requires hal_esp32 rtos-support statistics sitewhere actuators x_json syslog printf systiming commands )
idf_component_register( SRCS ${srcs}
						INCLUDE_DIRS ${include_dirs} 
#						PRIV_INCLUDE_DIRS ${priv_include_dirs} 
						REQUIRES ${requires} 
						PRIV_REQUIRES ${priv_requires}
						)
