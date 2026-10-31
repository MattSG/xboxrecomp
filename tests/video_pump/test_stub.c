#include "video/video_player.h"
int main(void) {
    xbox_VideoSetPresentHandler(0);
    return xbox_VideoPlayFile("unsupported") != -1 || xbox_VideoIsPlaying() ||
        video_dump_frame_bmp("unsupported") != -1;
}
