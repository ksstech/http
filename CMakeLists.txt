# HTTP

set( srcs "x_http_client.c" "x_http_common.c" "x_http_server.c" "${PRepo}/libyuarel/yuarel.c" "${PRepo}/http-parser/http_parser.c" )
set( include_dirs "." "${PRepo}/libyuarel/." "${PRepo}/http-parser/." )
set( priv_include_dirs )
set( requires "buffers socketsX stringsX" )
set( priv_requires "confidential irmacos jsonX rules printfx syslog systiming" )

idf_component_register(
	SRCS ${srcs}
	INCLUDE_DIRS ${include_dirs}
	PRIV_INCLUDE_DIRS ${priv_include_dirs}
	REQUIRES ${requires}
	PRIV_REQUIRES ${priv_requires}
)
