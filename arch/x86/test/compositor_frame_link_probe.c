/* Keep this separate: a same-file test would miss the original static symbol. */
extern void compositor_request_frame(void) __attribute__((weak));

int compositor_frame_external_request(void)
{
    if (!compositor_request_frame) return 0;
    compositor_request_frame();
    return 1;
}
