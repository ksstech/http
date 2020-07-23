idf_component_register(
	SRCS "x_http_client.c" "x_http_common.c" "x_http_server.c"
	INCLUDE_DIRS "."
	REQUIRES http-parser irmacos libyuarel
	PRIV_REQUIRES commands printf rtos-support rules sitewhere stringsX systiming x_json
)
