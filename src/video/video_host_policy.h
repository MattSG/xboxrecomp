/* Default host policy for guest-opened video files.
 *
 * With RECOMP_FMV_HOST set, a .wmv the guest opens is played by the host player
 * when nothing else is playing. A guest that returns to firmware while the
 * video plays waits for it to finish (up to a minute) first. A title that wants
 * a different policy skips this call and sets its own observer and hook. */
#ifndef VIDEO_HOST_POLICY_H
#define VIDEO_HOST_POLICY_H

void xbox_video_host_install_wmv_policy(void);

#endif
