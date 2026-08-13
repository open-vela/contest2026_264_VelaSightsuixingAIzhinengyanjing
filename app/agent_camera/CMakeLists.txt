# ai_agent camera-path bench (mirrors packages/ai_agent tool_camera.c)
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_AGENT_CAMERA)
  nuttx_add_application(
    NAME agent_camera
    SRCS agent_camera_main.c
    STACKSIZE 4096)
endif()
