#ifndef QUARREL_IMAGES_H
#define QUARREL_IMAGES_H

#include <civue/images.h>

/* ========================================================================== */
/* civue's images fetched through http, for avatars, icons, emoji and         */
/* attachments, kept on disk under the user's caches. main thread only.       */
/*                                                                            */
/*   civue_image_info avatar = civue_images_get(images, ui, url, 80);         */
/* ========================================================================== */

// one for the app. freed with civue_images_free, before http_shutdown.
civue_images *images_new(void);

#endif
