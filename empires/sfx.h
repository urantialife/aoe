/* Copyright 2018 the Age of Empires Free Software Remake authors. See LEGAL for legal info */

/**
 * Audio subsystem
 *
 * Licensed under Affero General Public License v3.0
 * Copyright by Folkert van Verseveld.
 *
 */

#ifndef SFX_H
#define SFX_H

#include <AL/al.h>
#include <AL/alc.h>
#include <SDL2/SDL_mixer.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_CHANNEL_COUNT 2

void sfx_init(void);
void sfx_free(void);

#ifdef __cplusplus
}
#endif

#endif
