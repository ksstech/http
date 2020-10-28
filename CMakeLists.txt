idf_component_register(
	SRCS "x_http_client.c" "x_http_common.c" "x_http_server.c"
		"libyuarel/yuarel.c"
		"http-parser/http_parser.c"
	INCLUDE_DIRS "."
		"libyuarel/."
		"http-parser/."
	REQUIRES irmacos
	PRIV_REQUIRES commands printfx rtos-support rules sitewhere stringsX systiming x_json
)
