/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Photo enhancement for t510-camera: a learned colour and tone retouch
 * (camera-enhance.c).
 */
#ifndef T510_CAMERA_ENHANCE_H
#define T510_CAMERA_ENHANCE_H

#include <glib.h>

/* Where t510-support puts the model */
#define ENHANCE_MODEL	"/usr/share/t510/models/lutbgrid-fivek.onnx"

/* TRUE when ONNX Runtime and the model are there; loads them once */
gboolean enhance_available(void);

/*
 * Retouch a packed RGB picture in place; FALSE (and the picture as it was)
 * when that cannot be done. Takes about a second for 8 MP.
 */
gboolean enhance_rgb(guint8 *data, int width, int height, int stride);

#endif
