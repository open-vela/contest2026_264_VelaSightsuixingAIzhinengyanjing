# BK7258 V4L2 M2M JPEG encoder test
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_JPEG_TEST)
  nuttx_add_application(
    NAME jpeg_test
    SRCS jpeg_test_main.c
    STACKSIZE 8192
    # Decoding a frame back for display needs libjpeg here too.  Its headers
    # are published with nuttx_export_header(), which lands them under the apps
    # binary dir rather than on the default application include path.
    INCLUDE_DIRECTORIES ${NUTTX_APPS_BINDIR}/include/turbojpeg-static
    DEPENDS turbojpeg-static)
endif()
