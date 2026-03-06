# DeepStream-Yolo-Face Pipeline Changes for Frame Saving

BEFORE:
  uridecodebin ──[pad-added]──────────────────────► nvstreammux ──► nvinfer ──► ...

AFTER (when frame_save.enabled):
  uridecodebin ──[pad-added]──► src_tee_N ─src_0─► nvstreammux ──► nvinfer ──► ...
                                            │
                                            └─src_1─► src_nvvidconv_N
                                                      ► src_capsfilter_N (RGBA NVMM)
                                                      ► src_queue_N (leaky/drop)
                                                      ► src_appsink_N  ←── raw_src_appsink_callback