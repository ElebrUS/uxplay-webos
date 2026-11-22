/* Plays an Annex B H.264 fixture with a four-byte AUD before each access unit. */
#include <lgnc_directvideo.h>
#include <lgnc_plugin.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { WIDTH = 640, HEIGHT = 360, MAX_FIXTURE = 512 * 1024 };

static size_t next_aud(const unsigned char *data, size_t size, size_t start) {
    for (size_t at = start; at + 4 < size; ++at) {
        if (!data[at] && !data[at + 1] && !data[at + 2] &&
            data[at + 3] == 1 && (data[at + 4] & 31) == 9) return at;
    }
    return size;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    static unsigned char bytes[MAX_FIXTURE];
    FILE *fixture = fopen(argv[1], "rb");
    if (!fixture) return 2;
    const size_t size = fread(bytes, 1, sizeof(bytes), fixture);
    const int read_failed = ferror(fixture);
    fclose(fixture);
    if (read_failed || size == sizeof(bytes) || size < 5 || next_aud(bytes, size, 0) != 0) return 2;

    LGNC_CALLBACKS_T callbacks = {0};
    int rc = LGNC_PLUGIN_Initialize(&callbacks);
    fprintf(stderr, "LGNC Initialize: %d\n", rc);
    if (rc) return 1;
    rc = LGNC_PLUGIN_SetAppId(argv[2]);
    fprintf(stderr, "LGNC SetAppId: %d\n", rc);
    if (rc) {
        LGNC_PLUGIN_Finalize();
        return 1;
    }
    const LGNC_VDEC_DATA_INFO_T video = {WIDTH, HEIGHT, LGNC_VDEC_FMT_H264, LGNC_VDEC_3D_TYPE_NONE};
    rc = LGNC_DIRECTVIDEO_Open(&video);
    fprintf(stderr, "LGNC Open: %d\n", rc);
    if (rc) {
        LGNC_PLUGIN_Finalize();
        return 1;
    }
    rc = _LGNC_DIRECTVIDEO_SetDisplayWindow(0, 0, 1920, 1080);
    fprintf(stderr, "LGNC SetDisplayWindow: %d\n", rc);
    size_t start = 0;
    unsigned frames = 0;
    struct timespec interval = {0, 33333333};
    while (!rc && start < size) {
        const size_t end = next_aud(bytes, size, start + 5);
        for (unsigned retry = 0; retry < 10; ++retry) {
            rc = LGNC_DIRECTVIDEO_Play(bytes + start, (unsigned)(end - start));
            if (rc != LGNC_BUFFER_FULL) break;
            nanosleep(&interval, NULL);
        }
        if (rc) break;
        ++frames;
        start = end;
        nanosleep(&interval, NULL);
    }
    fprintf(stderr, "LGNC Feed: %u accepted, last status %d\n", frames, rc);
    struct timespec hold = {2, 0};
    if (!rc) nanosleep(&hold, NULL);
    LGNC_DIRECTVIDEO_Close();
    LGNC_PLUGIN_Finalize();
    return rc || frames < 2;
}
