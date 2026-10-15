pl_mpeg.h: the MPEG-1 video decoder and MPEG-PS demuxer by Dominic Szablewski (MIT). Used by port/src/plat_movie.c
for the movies as port/tools/dc_movie.sh converts them (the disc's PSS files are MPEG-2).

  pl_mpeg.h     upstream, https://github.com/phoboslab/pl_mpeg: the PC (the PVR renderer of the simulation)
  pl_mpeg_dc.h  the Dreamcast's, from Quake2-main (client/pl_mpeg.h): the SH-4's FIPR in the IDCT, and each
                picture also kept as the PVR's YUV converter takes it (frame->display: 16 x 16 macroblocks of 384
                bytes, the converter's 4:2:0 layout). Its file and memory calls are the MPEG_* macros its player
                defines (plat_movie.c defines them for KOS).
Both unchanged.
