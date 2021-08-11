# HTTP Server/Client
set( srcs "x_http_client.c" "x_http_common.c" "x_http_server.c"
		"${PRepo}/libyuarel/yuarel.c" "${PRepo}/http-parser/http_parser.c" )
set( include_dirs "." "${PRepo}/libyuarel/." "${PRepo}/http-parser/." )
set( requires buffers common hal_esp32 rtos-support socketsX stringsX ) 
set( priv_requires irmacos jsonX printfx rules syslog systiming )
idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires} PRIV_REQUIRES ${priv_requires})
