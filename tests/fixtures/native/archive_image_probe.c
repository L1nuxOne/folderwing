/* Harness-only observation of the real image reader's received pixel buffers.
 * This TU replaces app/imageview.c only in archive_tests.exe, never the app. */
#include "../../../app/imageview.c"
int archive_test_image_ready(void)
{
    return R.opened && !R.gone && SUCCEEDED(R.err) && (R.sharp.bitmap || R.thumb.bitmap);
}
